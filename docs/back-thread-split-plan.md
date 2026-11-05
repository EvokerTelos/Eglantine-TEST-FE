# Plano: tirar trabalho da thread de trás da GPU (Fortune's Weave)

Escrito em 2026-11-04 sobre o `main` em `43a7f26`, a partir do pré-plano
(`docs/back-thread-split-preplan.md`). Foco: tempo, sincronização e locks.

**Revisão 2 (2026-11-04):** incorpora a revisão adversarial estática sobre `91ee47a`. Todas as afirmações dela
foram conferidas no código. O que mudou está na seção 12; as seções 0, 1, 3, 4, 5, 7 e 11 foram corrigidas no
lugar.

## 0. Resumo

1. **Fase 0 (medir)** fica como no pré-plano, com mais contadores: tempo do worker do Vulkan (ocupado e
   ocioso), atraso do worker em draws e em frames, espera e posse dos locks dos dois lados, e o caminho de
   descritores de cada draw (descriptor buffer, push ou set), com a taxa de reaproveitamento e os bytes
   copiados e comparados por draw. Ela decide o resto.
2. **A opção A como está no pré-plano (uma thread nova dona do scheduler) fica rejeitada como primeiro
   passo.** Não é impossível, mas é cara: há ~220 chamadas ao scheduler fora do draw (caches, query cache,
   blits, compute pass, staging, present), várias com retorno usado na hora (`Finish`, `Wait`, `CurrentTick`,
   `UpdateGraphicsPipeline`, `IsRenderPassActive`). Ela exigiria separar o estado lógico do produtor (render
   pass, pipeline ligado, rescale, chunk do descriptor buffer) da emissão, ou transportar operações ordenadas
   com respostas. Detalhes na seção 1.
3. **Proposta para a fase 1:** o mesmo trabalho que A queria mover, sem mudar o dono do scheduler:
   - **1a, eliminar em vez de mover:** comparar o payload de descritores por identidade (ids de conteúdo)
     em vez de `memcpy` + `memcmp` de ~75 KB por draw com banco de texturas. Sem thread nova.
   - **1b, emissão no worker:** a escrita do descriptor buffer (`WriteDescriptorBuffer`, um
     `vkGetDescriptorEXT` por entrada) vai para dentro do comando gravado, e quem a executa é o
     `VulkanWorker`, que já consome os comandos em ordem. A ordem com fences, queries, `Drain`, present e
     `TickFrame` não muda, porque tudo continua no mesmo fluxo de chunks.
   - **1c (último recurso, só se o worker saturar):** um estágio de preparo entre a thread de trás e o worker, no mesmo fluxo
     de chunks (3 estágios), em vez de uma fila paralela.
4. **Fase 2 (B)** só com os números da fase 1. O ganho grande (×1,3-1,5) exige mover o rasterizador inteiro
   ou dividir o `PrepareDraw`. As duas formas e as armadilhas já vistas no código estão na seção 9.

Expectativa honesta: a fase 1 deve mover ou eliminar ~5-12% da thread (×1,05-1,14), e só nos draws com banco de
texturas no caminho de descriptor buffer. É a parte segura. O salto para 30 FPS na praça depende da fase 2.

## 1. O que a leitura do código muda no pré-plano

| Fato | Onde | Consequência |
| --- | --- | --- |
| ~220 chamadas de produtor ao scheduler fora de `ConfigureDraw`: `vk_rasterizer.cpp` 57, `vk_query_cache.cpp` 30, `vk_texture_cache.cpp` 30, `vk_buffer_cache.cpp` 26, `blit_image.cpp` 18, `vk_compute_pass.cpp` 15, `renderer_vulkan.cpp` 11, `vk_swapchain.cpp` 8 etc. | grep de `scheduler.(Record\|Request\|Flush\|Finish\|Wait\|Update\|CurrentTick)` | "`scheduler.Record` só na thread de emissão" (fase 1 do pré-plano) obriga a rotear todas elas. As que precisam de retorno na hora teriam de esvaziar a thread de emissão antes, o que serializa tudo de novo. |
| `DescriptorBufferRing::Allocate` chama `scheduler.Wait`, `scheduler.Finish` e `CurrentTick` | `vk_descriptor_buffer.cpp:95-130` | A alocação fica com quem é dono do scheduler (a thread de trás). Só a **escrita** do conteúdo pode sair. |
| `UpdateDescriptorQueue::Acquire` faz `WaitWorker` no estouro e, ao crescer, **realoca** o vetor `payload` | `vk_update_descriptor.cpp:42-68` | Hoje é seguro porque o único leitor do payload é o worker e ele já foi esperado. Uma thread nova lendo o payload teria de ser esperada também, em todo `WaitWorker`. |
| O caminho de descriptor set (`UpdateDescriptorSet`) e o de push descriptor já rodam **dentro do lambda**, no worker. Só o caminho de descriptor buffer escreve na thread de trás. | `vk_graphics_pipeline.cpp:1012-1029` e `:1065-1114` | Com descriptor buffer (provável na RTX para os pipelines grandes: >32 descritores excluem o push descriptor), a escrita é a parte movível que sobra. A fase 0 confirma qual caminho é usado. |
| No acerto do cache de descritores da tabela compacta, `AddEntries` copia `desc.count` entradas de 24 B; depois `ConfigureDraw` compara o payload inteiro com `last_descriptor_payload` (`memcmp`) e, na falha, copia de novo (`assign`) | `pipeline_helper.h:483`, `vk_graphics_pipeline.cpp:1004-1028` | Com ~3100 entradas por draw (`reserve=3112` no log antigo): ~75 KB copiados + ~75 KB comparados por draw com banco, mesmo quando nada mudou. Isso pode ser eliminado (1a). |
| `DelayedDestructionRing` conta **frames** (`TickFrame`), não ticks da GPU: views e imagens apagadas morrem 8 `TickFrame` depois. As 8 fatias do payload também são reusadas por `TickFrame`, sem espera (`vk_update_descriptor.cpp:32-37`) | `texture_cache_base.h:546-552`, `delayed_destruction_ring.h` | A segurança de quem lê mais tarde vem do present: o pool tem até **7** frames (`MAX_FRAMES_IN_FLIGHT`, `vk_present_manager.cpp:25`, `:367`), e um frame só volta ao pool depois de o worker executar o comando de present do composite que o usou. Isso dá margem **exata** (N ≤ 7 contra 8 ticks), sem folga. Demonstração em 5.2. 1b não muda a conta. Um estágio novo (1c) não a quebra por si só, mas exige demonstrar que nenhum present ultrapassa preparo pendente (5.2). |
| `DispatchWork` a cada 32 draws, `Flush` a cada 4096 draws ou 192 operações/2,5 ms | `vk_rasterizer.cpp:1013-1023`, `:1124-1144` | É a cadência de entrega ao worker, **não um limite de profundidade**: `DispatchWork` só empilha chunks (`vk_scheduler.cpp:88-102`), e a fila pode acumular muitos se o worker atrasar. |
| O compute também consome `guest_descriptor_queue` e escreve o descriptor buffer na hora, na thread de trás | `vk_compute_pipeline.cpp:282-292` | Qualquer payload incompleto (buracos de 1a) tem de ser proibido fora do graphics, por autorização explícita, não pelo modo da fila. (Hoje o compute não passa `compact` ao `PushImageDescriptors`, `:265`, mas a regra não pode depender disso.) |
| O lambda de draw tem `return` antecipado (`if (!pipeline) return;`) | `vk_graphics_pipeline.cpp:1076` | Em 1b, a escrita do descriptor buffer tem de ser a **primeira** coisa do lambda, antes de qualquer retorno. Senão a thread de trás publica uma alocação como reaproveitável que nunca foi escrita. |
| Esperas dentro dos locks dos caches: `DownloadMemory` → `runtime.Finish()` | `buffer_cache.h:2154`, chamado no `ConfigureImpl` | Com o lock dos dois caches seguro, a thread de trás pode esperar o worker e a GPU, e as threads do jogo esperam junto. Mais trabalho no worker prolonga essa espera (H8). `DescriptorBufferRing::Allocate` também espera sob o lock. |
| Os dois mutexes dos caches são `std::recursive_mutex`; o query cache pega `buffer_cache.mutex` por dentro | `texture_cache_base.h:297`, `buffer_cache_base.h:365`, `vk_query_cache.cpp:1410` etc. | Estreitar o lock de `PrepareDraw` (item L) exige conferir cada chamada que hoje está dentro dele. |
| `Finish` por frame no `DownloadMemory` da tabela de handles | `vk_graphics_pipeline.cpp:~501`, perf-handoff | É um ponto serial: a thread de trás espera o worker e a GPU. Tudo que for para o worker volta para o caminho crítico nesse ponto, na medida da fila do worker. Ver H8. |

Por que não começar pela "thread de emissão dona do scheduler": um proxy ingênuo (`Record` na thread de trás vira
um item de fila que a thread de emissão passa para o chunk) faz o que o chunk já faz, com um salto a mais. Uma
versão útil precisaria separar o estado lógico do produtor (que continuaria na thread de trás, respondendo às
consultas síncronas) da emissão, e isso mexe nas ~220 chamadas. Nada disso é impossível, mas o trabalho que ela
tiraria da thread de trás na fase 1 é o mesmo que o lambda pode fazer no worker. Se o worker não tiver folga, o
próximo passo é 1c (um estágio no mesmo fluxo), não uma fila paralela com protocolo próprio de ordem.

## 2. Arquitetura proposta para a fase 1

```
front ──ring gpu_split──▶ back (dono dos caches e do scheduler) ──chunks (fila atual)──▶ VulkanWorker ──▶ vkQueueSubmit
                           resolve ids, handles, alocação                                 executa lambdas:
                           decide reuso e bind                                            escreve descriptor buffer (1b)
                           grava lambdas                                                  vkCmd*
```

Regras que valem para toda a fase 1:
- **R1. O scheduler tem um produtor por vez: a thread de trás com o split ligado, a thread da GPU sem ele.**
  Nenhuma chamada nova de `Scheduler::*` fora do produtor. A troca de produtor é legítima e sincronizada
  (`GpuSplit::MaybeStart` drena e para o split, e pode criar outra thread depois, `gpu_split.cpp:59-86`).
  A verificação acompanha isso (H1).
- **R2. O worker não lê caches nem pega locks de cache.** O lambda só recebe dados prontos: ponteiros para
  memória com tempo de vida garantido (seção 7, H2) e valores capturados.
- **R3. Toda decisão que afeta a submissão ou o draw é tomada na thread de trás:** pular o draw, falha de
  alocação, reaproveitamento da alocação, bind do chunk do descriptor buffer, pipeline e rescale. O worker
  só executa.
- **R4. Interruptor fixo por processo** (lido uma vez, em `static const`): `EDEN_DRAW_EMIT=0` volta ao caminho
  atual; `=1` só 1a; `=2` 1a+1b (padrão depois de validado). Sem troca no meio da sessão, porque o estado de
  reaproveitamento por pipeline (`last_descriptor_payload` e a chave nova) depende do modo.

## 3. Fase 0: medir (um build, sem mudança de comportamento)

### 3.1 Como

- Interruptor próprio, `EDEN_BACK_THREAD_STATS=1`, separado do `EDEN_TEXTURE_TABLE_STATS`. Os
  temporizadores custam ~1-2% da thread, e a métrica de comparação (draws/s da linha `Texture tables`) deve
  ser lida com eles desligados. Desligado, custa um `if` com `static const bool` por ponto.
- Relógio: `__rdtsc()` (MSVC `<intrin.h>`, GCC `<x86intrin.h>`); o Zen 3 tem TSC invariante. Calibrar contra
  `steady_clock` no relatório (ticks/µs medidos entre dois relatórios). Fora de x86, `steady_clock`.
  **Limite de precisão:** `rdtsc` sem serialização pode ser reordenado em dezenas de ciclos. Calibrar a
  frequência não resolve isso. Serve para baldes acumulados de milhares de trechos de µs, não para trechos
  curtos isolados. Regras: baldes abaixo de ~1% da thread não decidem nada; um balde que decide um limiar da
  tabela 3.5 é conferido com uma rodada usando `_mm_lfence(); __rdtsc()` nas suas duas fronteiras e com o
  perfil por amostragem do VS. O custo dos próprios carimbos é medido comparando draws/s com a estatística
  ligada e desligada.
- **Carimbos encadeados**, um por fronteira: `t = Mark(bucket, t)` soma `now - t` no balde e devolve `now`.
  Cada trecho cai em um balde só, então os baldes somam o total, e "outros" = ocupado − soma. Nada de RAII
  aninhado (contaria duas vezes e esbarra no `/we4456` com nomes repetidos).
- Acumuladores locais não atômicos só enquanto pertencem a uma única thread. **Publicação:** cada thread que não
  emite o relatório (worker, threads do jogo, frente) escreve em `std::atomic<u64>` próprios. O worker soma
  localmente e publica com `store(relaxed)` uma vez por chunk; as threads do jogo fazem `fetch_add(relaxed)`
  só com o interruptor ligado. A thread de trás lê esses atômicos com `load(relaxed)`. Para estatística basta:
  cada valor é atômico em si, e uma defasagem de um chunk entre contadores não importa.
- O número do frame atual da thread de trás que o worker compara (3.4) também é um `std::atomic<u64>`
  (`store(relaxed)` no `TickFrame`, `load(relaxed)` no worker).
- Relatório a cada 4 s, emitido pela thread de trás, como a linha `Texture tables`.

### 3.2 Baldes da thread de trás (µs/s e % do tempo ocupado)

1. `BackLoop`: ocupado (dentro de `Execute`) e ocioso (espera e sono): dá o total real e mostra se a thread
   é mesmo 100%.
2. Fora de draws: `CallMethod` e `CallMultiMethod` que não chegam a `ProcessDraw`, com uploads inline e DMA à
   parte (`AccelerateDMA::*`, `AccelerateInlineToMemory`).
3. `PrepareDraw`: `FlushWork` + `FlushCaching`; `CurrentGraphicsPipeline`; **espera pelo lock**
   (antes e depois do `scoped_lock`); dentro do lock: `ConfigureImpl` dividido em
   (a) VPRS e leitura de handles e `Build` (até `FillImageViews`); (b) resolução de views e samplers e bancos;
   (c) `bind_stage_info` e texture buffers; (d) `UpdateGraphicsBuffers` + `BindHostGeometryBuffers`;
   (e) por estágio, `BindHostStageBuffers` e `PushImageDescriptors` separados, e dentro do segundo os contadores
   de acerto, patch e falha de bloco compacto e do caminho elemento a elemento; (f) `PostCopyBarrier`,
   `UpdateRenderTargets`, `CheckFeedbackLoop`; (g) `ConfigureDraw` dividido em comparação do payload,
   `Allocate`, `WriteDescriptorBuffer`, `assign` e `Record`. Depois: `UpdateDynamicStates`, queries e
   transform feedback, `draw_func`, `FlushStorageWrites`. Por último, **posse do lock** (da aquisição à
   liberação).
4. `TickWork` no `SCOPE_EXIT` e `RendererVulkan::Composite` (present), à parte.
5. Esperas no scheduler vistas pela thread de trás: `WaitWorker`, `Finish` (com quem chamou:
   `DownloadMemory`, ring de descritores esgotado, texture cache) e `Wait(tick)`.

### 3.3 Contadores de descritores (por segundo)

- Draws por caminho: descriptor buffer, push, set, sem layout. Para descriptor buffer: alocação
  reaproveitada contra nova, entradas por draw, chamadas a `vkGetDescriptorEXT`, "frame exhausted"
  (`Finish` no ring) e tempo da espera de `frame_reused`.
- Bytes de `AddEntries`, bytes de `memcmp` em `ConfigureDraw`, bytes de `assign`.
- Estouros e crescimentos da fila de payload (já logados; aqui em contagem por segundo).

### 3.4 Worker, locks e frente

- Worker: tempo em `ExecuteAll` contra espera; chunks por segundo; profundidade da fila no `DispatchWork`
  (`work_queue.size()` já está sob `queue_mutex`), em média **e em máximo por janela**. A média de ocupação
  esconde rajadas: um frame com muitos bancos novos pode encher a fila logo antes de um `Finish`.
- **Fila no instante de cada ponto de sincronização:** em cada `Finish`, `WaitWorker` e `Wait(tick)` da
  thread de trás, registrar (a) os chunks ainda na fila do worker, (b) se o chunk atual do produtor tinha
  trabalho retido (não despachado), e (c) quanto tempo a espera durou. Isso separa dois casos com remédios
  diferentes: worker ocioso com trabalho retido no produtor (despachar mais cedo ajuda) contra worker saturado
  (não ajuda).
- **Atraso do worker em frames (diagnóstico, não prova):** `CommandChunk` ganha (só com estatística ligada) o
  número do frame (`TickFrame`) da thread de trás quando recebeu o **primeiro** comando (no `AcquireNewChunk`
  ou no primeiro `Record`, não no despacho). O worker registra o máximo de `frame_atual_de_trás −
  frame_do_chunk`: é a idade do comando mais antigo que ele executa. A segurança vem da demonstração de 5.2;
  este número só ajuda a entender um problema em campo.
- Atraso da submissão: do `SubmitExecution` gravado ao `vkQueueSubmit` executado (o lambda de submissão
  carimba).
- Threads da CPU do jogo: contagem e tempo de espera nos locks de `OnCPUWrite`, `OnCachedWrites`,
  `OnCacheInvalidation`, `InvalidateRegion` e `FlushRegion` (atômicos relaxed). Na thread de trás, só o tempo
  antes e depois do `scoped_lock` (sem `try_lock`: `scoped_lock` com dois mutexes usa `std::lock`, e trocar a
  forma de aquisição muda o que se mede).
- Tempo que a thread de trás passa **esperando dentro do lock dos caches** (`Finish` do `DownloadMemory`,
  `Wait` do `Allocate`), à parte do tempo de trabalho dentro do lock: é o que as threads do jogo também
  esperam.
- Frente: tempo em `Drain` e `WaitForSpace`, para confirmar que a frente nunca é o limite.

### 3.5 Saída e decisão

O usuário roda a praça (pior ponto, depois de estabilizar ~30 s), o palácio e uma batalha, e manda o log. A
sessão monta a tabela do pré-plano com números reais e decide pela tabela abaixo (limiares em % do tempo
ocupado da thread de trás):

| Condição medida | Ação |
| --- | --- |
| cópia + comparação de payload em acertos ≥ 3% | 1a |
| `WriteDescriptorBuffer` sozinho ≥ 3% (o `assign` **fica** na thread de trás e não conta) **e** worker ocupado ≤ 40% em média **e** sem rajadas que encham a fila antes dos `Finish` | 1b |
| worker ocupado > 40% já hoje, ou rajadas grandes na fila antes dos `Finish` | antes de 1b, medir o cache de bytes por bloco (pergunta 5); 1c só como último recurso (ver 7, H3) |
| espera da thread de trás pelo lock ≥ 3% | item L (seção 6) |
| caminho push ou set em draws com banco ≥ 30% desses draws | estender 1a/1b a eles (seção 5.3) |
| `Finish` por frame ≥ 1 ms | anotar: limita o ganho de 1b (H8) |
| (a) + `CurrentGraphicsPipeline` + estados dinâmicos + fora de draws ≥ 35% | B vale um estudo (seção 9) |
| 1a + 1b juntos < 5% | não fazer a fase 1; ir direto ao desenho de B ou C |

### 3.6 Como rodar (implementado no branch `perf/back-thread-split`)

- `EDEN_BACK_THREAD_STATS=1` (ou `=2`, carimbos com `lfence`) e `EDEN_TEXTURE_TABLE_STATS=1` na mesma janela do
  PowerShell. Seis linhas a cada 4 s: `Back thread:` (draws/s, ocupado, `PrepareDraw`, composite, clear, compute,
  DMA, upload inline, lock segurado), `Back thread draw spans` (os 21 trechos encadeados, % do tempo de parede),
  `Back thread descriptors`, `Back thread waits` (`Finish`, `WaitWorker`, `Wait`; fila e retido no produtor),
  `Vulkan worker` e `Cache locks` (com a frente do split).
- Rodada: praça (vista aberta, ~30 s parado depois de estabilizar), palácio de Megaira, uma batalha. Uma rodada
  com `=1` e uma curta com `=2` na praça. Draws/s para comparar builds: com `EDEN_BACK_THREAD_STATS` desligado.
- Os trechos somam ~o `PrepareDraw` (o resto é o próprio custo dos carimbos). "other busy" = ocupado da thread
  de trás menos draws, composite, clear e compute: métodos da engine fora de draws, macros da frente
  replicadas, `TickWork`, DMA e uploads inline (estes dois também aparecem à parte).
- Limitações: os carimbos custam ~20 por draw (~1-2%); `Wait(tick)` também conta as esperas de dentro do
  `Finish`; "in draw lock" só vale para o lock de `PrepareDraw`, não para o de `Clear`/`DispatchCompute`/DMA.

### 3.7 Rodada 1 (praça, 2026-11-04, `0ce86c7`)

Duas janelas de 4 s, `=1` e `=2` (concordam dentro de ~1 ponto): 68,8-71 mil draws/s (a medição custa pouco:
~72 mil sem ela).

| Medido | Valor | Leitura |
| --- | --- | --- |
| Thread de trás ocupada / ociosa | 86% / 13-14% | **Não está a 100%.** Ela fica sem trabalho 13% do tempo. |
| Frente: `Drain` que esperou | ~810-840/s (~27 por frame), 70% do tempo da frente | Cada `Drain` esvazia o ring; quando a frente volta, a de trás está ociosa até chegar trabalho. Provável origem da ociosidade: até ×1,15 se as bolhas sumissem. Rodada 2 diz quem drena. |
| Caminho de descritores | buffer **0/s**, push 62-64 mil/s, set 6,9-7,1 mil/s (≈ os draws com tabela) | **1a e 1b, como desenhados, não se aplicam nesta máquina:** nenhum draw usa descriptor buffer. Os draws com banco vão por descriptor set (escrita já no worker). |
| `desc compare` (+ `assign`, caminho set) | 1,4-1,5% | Pouco. O `memcpy` dos acertos (`AddEntries`) está dentro de `push images` (8,0-8,4%). |
| Worker do Vulkan | 24% ocupado, fila média 0,4-0,5 (máx. 10-16), comando mais velho ≤ 1 frame, gravação → submit ~170 µs | Folga. |
| `Finish` | ~21/s (1 por frame), 37-40 ms/s (~4%), **todo dentro do lock do draw** | O `DownloadMemory` da tabela de handles. As threads do jogo esperam junto. |
| `Wait(tick)` | 185-191/s, 440-450 ms/s | Grande, mas sem atribuição de thread (provavelmente as threads de fence). Rodada 2 separa. |
| Espera da thread de trás pelo lock | `lock wait` 4,1-4,3% (+ `tail` 3,1-3,7%, que inclui soltar o lock) | ≥ 3%: o item L passa no critério. |
| Threads do jogo nos locks | `OnCPUWrite` ~140 mil/s, **~1,2 s/s de espera somada** | Disputa forte: o draw segura os locks 43-45% do tempo. |
| Maiores trechos | handles 13,8-14,9, flush work 8,4, push images 8,0-8,4, stage buffers 6,9-7,2, buffers 5,9-6,2, resolve 3,8-4,4 | `flush work` (8,4%) é inesperado; a rodada 2 separa `FlushWork` de `FlushCaching`. |
| Fora dos draws | other busy 19-20%, inline upload 3,5-3,8%, compute 3,0-3,7% | Métodos da engine. |

Pela tabela 3.5: 1a/1b ficam suspensos (sem descriptor buffer); o item L passa (≥ 3%); B passa (handles +
pipeline + estados dinâmicos + fora de draws ≈ 36% ≥ 35%). Candidato novo, fora do plano original: **reduzir as
bolhas dos `Drain`** (até 13% de ociosidade). A rodada 2 (`EDEN_BACK_THREAD_STATS`, mesmo interruptor) acrescenta:
`Drain` por chamador, `Wait(tick)` na thread do draw contra as outras, `FlushWork` separado de `FlushCaching`,
`setup` e `storage bind` separados de `handles`, `unlock` separado de `tail`, e descriptor sets mantidos contra
reescritos por bind de pipeline.

### 3.8 Rodada 3 (praça, `ae52996` e `050a4e1`)

- `FlushCaching` (8,5%) aplica as escritas de constant buffer da engine (`ProcessCBMultiData` →
  `WriteBlockCached`) via `InnerInvalidation`, que pega e solta cada lock de cache **fora** do lock do draw.
- **Experimento F** (`EDEN_FLUSH_IN_DRAW_LOCK=1`, pegar o lock antes): `flush caching` 8,5 → 3,6%, mas `lock
  wait` 4,1 → 6,7% e `unlock` 3,3 → 5,4%; o custo por draw ficou igual (8,50-8,69 → 8,54 µs), o lock passou a
  ficar mais tempo seguro (44,6-45,6 → 47,1%) e a espera das threads do jogo em `OnCPUWrite` subiu de ~1,3 para
  ~2,1 s/s. O custo só mudou de lugar: **revertido** (`00dd52f`). Conclusão: o gargalo é a disputa com as threads
  do jogo (~135-140 mil `OnCPUWrite`/s, cada uma pegando `buffer_cache.mutex` só para consultar se a região é
  registrada e modificada pela GPU, `buffer_cache.h:159-169`), não o número de aquisições da thread de trás.
- **Acquires:** 743-788/s, todos de semáforo nativo, **100% prontos logo depois do `Drain`** (nenhum precisou
  esperar fences, nenhum suspendeu). A rodada 4 mede se o valor **já estava pronto antes** do `Drain` (leitura
  simples da memória do guest, só com a estatística ligada), se o `Drain` mudou o valor e o tipo de operação
  (igual, maior ou igual, máscara). Se estiver pronto antes e for "maior ou igual" sobre sequências crescentes,
  há um caminho para pular o `Drain` (ganho potencial: a ociosidade de 10-15% da thread de trás).
- `Wait(tick)` das outras threads: ~115-120/s, ~360-410 ms/s; não é a thread de trás.

### 3.9 Rodada 4 e o experimento D (pular o `Drain` do acquire)

Praça, `41a9366`: 802 acquires/s, **todos "maior ou igual"**; 294/s já prontos antes do `Drain`, 580/s com o
valor mudado pelo `Drain` (o release que os satisfaz estava na fila da thread de trás, enfileirado pela frente,
em geral por outro canal). Thread de trás ociosa 13%.

**D** (`EDEN_SKIP_ACQUIRE_DRAIN`, `puller.cpp`): a frente anota, por endereço de dispositivo, o payload de cada
release que enfileira para a thread de trás (`ProcessSemaphoreRelease` e o `WriteLong` do trigger). Num acquire
nativo "maior ou igual", se o último release anotado para o endereço desde o último `Drain` (época
`GpuSplit::DrainEpoch`) já satisfaz o valor, o `Drain` é pulado: a thread de trás executa o ring em ordem, então
o release roda antes de tudo que vem depois do acquire. Modo `2` também pula quando a palavra na memória já
satisfaz e não há release anotado.
- Por que é seguro: a ordem do que a GPU vê não muda (mesmo fluxo); a frente não usa o rasterizador depois de
  um acquire pronto; a tabela é só da frente e fica velha em todo `Drain` e `Stop` (a época sobe).
- O que pode quebrar: uma escrita na mesma palavra que a frente não vê (semáforo de engine 3D/compute/DMA na
  mesma posição, a CPU zerando o semáforo com trabalho ainda na fila) entre o release anotado e o acquire. Por
  isso cada acquire pulado agenda uma **checagem na thread de trás, no ponto exato do acquire no fluxo**
  (`ReadNativeSemaphoreWord`), que loga `Skipped acquire drain unmet in order` se não estiver satisfeito. O modo
  `2` tem o risco extra de a memória do guest estar velha em relação ao buffer da GPU.
- Ganho esperado: até a ociosidade da thread de trás (10-15%) mais o tempo de espera da frente.

### 3.10 Rodadas 5-6: o D não se aplica; a pausa pode ser latência de acordar

- `EDEN_SKIP_ACQUIRE_DRAIN=1` (confirmado no log) não pulou nenhum `Drain`. Os acquires que o `Drain` satisfaz
  (~580/s) esperam releases de **QMD de compute nativo** (448/s somam ao semáforo num callback de fence, quando
  a GPU termina o dispatch: `kepler_compute.cpp:170`; 51/s escrevem direto) e 81/s de origem desconhecida
  (colisão ou outro espaço de endereço). Nenhum vem do puller, do DMA nem do report 3D. A frente não tem como
  saber esse valor (vem da conclusão na GPU e do QMD na memória): **o D foi removido**. O canal gráfico espera
  mesmo o compute terminar na GPU.
- Thread de trás ociosa ~11,7% com ~780 `Drain`/s: ~150 µs de pausa por `Drain`. Hipótese: duas latências de
  acordar. Durante o `Drain` (~0,9 ms) a frente gira 16384 `pause` e dorme; quando a de trás termina, ela acorda
  a frente, que só então lê o semáforo e publica mais; enquanto isso a de trás gira 8192 `pause` e dorme também.
- Rodada 7 mede cada lado (dormiu?, latência de acordar, fim do `Drain` → próxima publicação) e traz o
  experimento `EDEN_SPLIT_SPIN=N` (giro N vezes maior nos dois lados). Custo: um núcleo girando enquanto espera;
  há folga (CPU ~33%), mas o giro no irmão SMT da thread de trás roubaria ciclos dela.

### 3.11 Rodada 7: não é latência de acordar

| Medido | Sem giro extra | `EDEN_SPLIT_SPIN=8` |
| --- | --- | --- |
| Draws/s | 74,5 mil | 75,1 mil |
| Thread de trás ociosa | 13,0% | 12,2% |
| Frente acorda depois da chamada | 6,6 µs | 6,1 µs |
| Fim do `Drain` → próxima publicação | 1,0 µs | 0,9 µs |
| Episódios de ociosidade da de trás | 2087/s, 62 µs cada | 2110/s, 58 µs cada |
| ... em que ela dormiu | 1% (acorda em 8,5 µs) | 1% |

A hipótese caiu: os dois lados acordam em poucos µs, e a de trás quase nunca dorme. Ela fica ociosa ~2,7 vezes
por `Drain`, ~60 µs cada, **girando à espera da frente**: nesses momentos a frente não tem trabalho para mandar.
`EDEN_SPLIT_SPIN` foi removido. (O "slept in 170% of drains" contava cada vez que a frente dormiu: ela acorda e
volta a dormir durante um `Drain`, porque a de trás devolve espaço do ring a cada 4096 palavras.)

Rodada 8 mede o tempo da frente esperando o jogo submeter command lists (`PopWait` em `gpu_thread.cpp`) e
dormindo com canal suspenso, e o tempo em que ela está ocupada (parsing, macros, envio). Se a frente espera o
jogo nesses intervalos, quem limita ali é a CPU do jogo (que espera os locks dos caches ~1,2 s/s).

### 3.12 Rodada 8: a ociosidade é a GPU emulada esperando o jogo

- A frente espera o jogo submeter ~26-27 vezes/s (≈ 1 por frame), **128-146 ms/s**, ~5 ms por frame; a thread
  de trás fica ociosa 128-142 ms/s e dorme ~21-24 vezes/s. A ociosidade (~13%) é a espera, a cada frame, pela
  próxima submissão do jogo, não os `Drain`. A frente fica ocupada só ~16-17% do tempo (o resto é `Drain`).
- Nesses ~5 ms por frame quem limita é a CPU do jogo, que perde ~1,1-1,2 s/s nos locks dos caches
  (`OnCPUWrite`, ~140 mil/s).
- `HandleRasterizerWrite` (`core/memory.cpp`) já guarda por núcleo a última página em que o `OnCPUWrite`
  respondeu "segura" e nunca a invalida; uma escrita segurada é aplicada antes da próxima submissão
  (`OnCachedWrites` → `CachedWriteMemory`, que também trata a região que deixou de ser da GPU). Rodada 9 mede os
  três resultados do `OnCPUWrite` (não registrada, segurada, marcada) e quantas seguradas cairiam nas 16 últimas
  páginas do núcleo, e traz o experimento `EDEN_CPU_WRITE_PAGES=N` (N páginas, mesmo contrato).

### 3.13 Rodadas 9-10: pacing da tela, `EDEN_CPU_WRITE_PAGES` e perfil

- Correção da 3.12: o jogo estava a 19-20 FPS (não 25), e a espera de ~5-6 ms por frame é, muito provavelmente,
  o encaixe da tela emulada (60 Hz, swap interval 2: um frame acima de 33 ms sai aos 50 ms). Com o VSync do PC
  desligado nada muda. A thread de trás trabalha ~43 ms por frame; para 30 FPS precisa ficar abaixo de 33 ms. A
  métrica passa a ser µs por draw e o % ocupado, não o idle.
- `OnCPUWrite`: held 111-124 mil/s, written 11-13 mil/s, unregistered 0; 96% das held caem nas 16 últimas
  páginas do núcleo. Com `EDEN_CPU_WRITE_PAGES=16`: `OnCPUWrite` 140 → 31 mil/s, `unlock` 3,2 → 1,5%, `flush
  caching` 8,1 → 6,9%, idle 12,4 → 7,3%, frente esperando o jogo 124 → 72 ms/s, draws/s +2,5%, FPS 19-20 → 20-21.
  Mas o `Finish` por frame (tabela de handles, dentro do lock do draw) subiu de 37 para 123 ms/s: com a thread de
  trás mais adiantada, ela espera mais a GPU nesse ponto (`handles` 12,0 → 20,8%; µs por draw 8,24 → 8,86).
- Perfil do VS (3 threads mais pesadas; 1% ≈ 2% da thread de trás): `memcpy` ~4%, `CompactTextureTable::Build`
  ~4%, `RtlReleaseSRWLockExclusive` ~4%, `BufferCache::IsRegionGpuModified` ~3,5% (chamado por
  `InnerInvalidation`, `AccelerateInlineToMemory`, `WriteMemory` e, menos, `CachedWriteMemory`: são as escritas
  de constant buffer e uploads inline da engine, não as escritas seguradas do jogo).
- Rodada 11 mede: o download da tabela de handles (tempo, draws gravados desde o último dispatch de compute, se
  os bytes mudaram desde o último download), os `Build` (quantos elementos mudam de fato) e as consultas
  `IsRegionGpuModified` (quantas dão verdadeiro, quantas cabem numa página). Candidatos: esperar só a cópia da
  tabela em vez do `Finish` inteiro; `Build` incremental; resposta por página para `IsRegionGpuModified`.

### 3.14 Visão geral depois das rodadas 1-10

**Meta em números.** Praça a ~20 FPS porque a thread de trás trabalha ~43 ms por frame (~3.600 draws); a tela
emulada (60 Hz, intervalo 2) só aceita 33 ou 50 ms. Para 30 FPS: abaixo de 33 ms, ou seja, **−25% de trabalho
por frame**. Ganhos menores só aparecem como média (22-24 FPS alternando).

**Onde vai o tempo da thread de trás (% do tempo de parede, ~87-93% ocupada):**

| Bloco | % | Natureza |
| --- | --- | --- |
| Fora de draws ("other busy") | ~20 | **Ainda não decomposto**; inclui trabalho do rasterizador (escritas seguradas no início de cada lista, fences e flush no fim, chamadas adiadas), não só a engine |
| Handles (inclui o `Finish` da tabela, 4-12%) | 12-21 | Memória do guest, `Build`, espera da GPU |
| `FlushCaching` (escritas de constant buffer da engine) | 7-8 | Invalidação + locks |
| Push images, stage buffers, buffers | ~21 | Caches de textura e buffer |
| Lock (esperar + soltar) | ~6-7 | Disputa com as threads do jogo |
| Resolve, pipeline, setup, comparações, resto | ~10 | Diversos |
| Compute, upload inline | ~8 | Engine |

**O que não é gargalo:** o worker do Vulkan (24%), a frente do split (~17% ocupada), os despertares (µs), a GPU
real em média (~67%).

**Descartado com dados:** 1a/1b (sem descriptor buffer na RTX), F (o custo do lock só mudou de lugar), D (os
releases vêm da conclusão de compute na GPU), giro maior (não era latência de acordar).

**Ganho até agora:** `EDEN_CPU_WRITE_PAGES=16`, +2,5% de draws/s (19-20 → 20-21 FPS), ainda sem validação visual
ampla.

**Restrições estruturais que apareceram:**
1. ~800 acquires/s esperam um release de QMD de compute que **a thread de trás** grava ao processar o dispatch.
   (Correção da revisão: com Fence Behavior Default e GPU fora de High, `SignalFenceCoalesced` roda o callback
   na hora, `fence_manager.h:80-89`; o "QMD add" não espera a GPU. O `Drain` espera a thread de trás chegar ao
   release, não a conclusão física do dispatch. Os contadores por origem só associam a mudança do valor ao último
   release anotado; não medem espera de GPU.) A frente continua sem saber o valor (vem do QMD lido na thread de
   trás). Cada acquire esvazia o split; qualquer estágio novo paga um esvaziamento a mais.
2. Uma vez por frame, a tabela de handles escrita por compute é baixada com `Finish` dentro do lock: a thread de
   trás espera a GPU ali, e mais quanto mais adiantada estiver.
3. A tela emulada quantiza: só um salto grande muda o FPS de forma estável.
4. **A GPU pode virar o limite perto de 30 FPS:** ~67% ocupada a 20 FPS significa ~100% a 30 FPS, nas mesmas
   configurações (a saída nativa é 3840x2160). A resolução interna vai pesar no último passo.

**Caminhos, em ordem de custo:**
- **Micro (2-5% cada):** reduzir o `Finish` da tabela (não é troca local: a cópia entra no command buffer
  principal depois de todo o trabalho anterior, `vk_buffer_cache.cpp:490-499`; só antecipar a cópia para logo
  depois do último escritor, evitar downloads redundantes ou reorganizar submissões reduz a espera); `Build` incremental; `IsRegionGpuModified` por página;
  cópias do caminho set; mais disputa de lock. Juntos talvez 10-15%: média 22-24 FPS, não 30.
- **Decompor os ~20% de "other busy"** antes de qualquer decisão estrutural: é o maior bloco sem diagnóstico.
- **Estrutural (B-split, rasterizador numa thread própria):** o teto de ~×1,4-1,5 **não está sustentado**: parte
  do "other busy" é do rasterizador e iria para a thread dele. E registradores não bastam: a engine grava
  memória do guest entre draws (`ProcessCBMultiData` → `WriteBlockCached`, `maxwell_3d.cpp:702-715`), e o
  rasterizador lê essa memória no draw (`FlushCaching`, `Configure`); um draw A ainda não preparado leria os bytes
  do draw B. O desenho teria de levar essas escritas no fluxo ordenado, versionar os dados ou sincronizar antes
  de sobrescrever; e preservar a fronteira de `ProcessDirtyRegisters` (draw adiado antes da escrita,
  `maxwell_3d.cpp:344-353`).
- **Heap bindless (C):** tiraria push images, resolve e parte de handles (~15-20%). Semanas no recompilador.

**Riscos de visão de túnel:** tudo foi medido só na praça (palácio, batalha e seleção ainda não); o
`EDEN_CPU_WRITE_PAGES` muda quando as escritas do jogo chegam aos caches da GPU (precisa de validação visual
ampla); e a medição custa ~1-2%.

### 3.15 Rodada 11 (revisada)

Além do que a 3.13 lista, a mesma rodada mede: o tempo da thread de trás por tipo de entrada do ring (método,
escrita de macro, método multi-palavra, chamada adiada), o `ConsumeSink`, o início de lista (escritas seguradas)
e o fim de lista (fences e flush) separados das demais chamadas; o resíduo da engine (métodos menos draws,
clears, compute, DMA e upload inline, agora também descontados do "other busy"); os sinais coalescidos que rodam
na hora contra os adiados para uma fence; e as leituras de semáforo que buscam o valor num buffer da GPU
(`FlushRegion`). A medição por entrada custa um carimbo por entrada do ring (~2-3% a mais com a estatística
ligada).

### 3.16 Revisão do PokeDinner (2026-11-04)

`git.eden-emu.dev` é bloqueado pela rede da sessão; lido o espelho `github.com/969981/PokeDinner-a-Eglantine-Fork`
(snapshot do master do Forgejo `216b5034c60a`, 3 de novembro), comparado com o espelho `pokedinner` deste
repositório (2 de novembro): ~13,5 mil linhas em 211 arquivos (boa parte de mesh shaders, upscale temporal,
memória esparsa do Switch 2, áudio). O que toca nosso gargalo:

| Mudança | O que faz | Situação aqui |
| --- | --- | --- |
| Upload inline adiado (`EDEN_NX2_INLINE_DEFER`, ligado lá) | Upload inline sem dados da GPU na região só escreve a memória e acumula a invalidação para o próximo `FlushCaching` ("milhares de escritas inline entre draws, ~10% da thread da GPU" no título deles) | Portado atrás de `EDEN_INLINE_DEFER=1` (desligado). Mesmo contrato das escritas de constant buffer: todos os consumidores chamam `FlushCaching` antes (draw, clear, compute, condicional, DMA, 2D, iguais nas duas bases); o acumulado é por canal |
| `ShaderCache::InvalidateRegions` | `InnerInvalidation` invalida o shader cache com um lock e uma passada de remoção para todas as regiões | Portado (mesma semântica, com o `DropRejections` que a nossa versão tem) |
| `RemovePendingShaders` com busca binária acima de 16 | Menos custo quando muitos shaders são removidos | Não portado (raro em regime) |
| `EDEN_NX2_FRAMES_AHEAD` (pacing) | Na apresentação do vi2 deles o jogo espera a fence do frame atual; com o modo ligado espera a do anterior (o jogo prepara N+1 enquanto a GPU emulada faz N), voltando a esperar o atual se a GPU estiver atrás | Não se transporta: nossa apresentação é o `BufferQueue` do nvnflinger (até 2 pendentes, sem espera direta). A pista vale: descobrir **o que o jogo espera** nos ~5-6 ms por frame em que a GPU emulada fica parada |
| `GpuSplit::WaitForWriters` | Espera parcial do split: só até o último compute/cópia enviado, para ler command lists escritas pela GPU | Não se aplica aos acquires (a leitura do semáforo usa o rasterizador, que precisa da thread de trás parada) |
| Upscale temporal (`AccelerateTemporalUpscale`) | Substituto do DLSS do Switch 2 via Fermi2D | Fora do escopo; pode importar quando a GPU real virar o limite perto de 30 FPS |

### 3.17 PokeDinner, com a história (servidor liberado)

Com o `git.eden-emu.dev` liberado, a história desde o espelho de 2 de novembro tem 499 commits. Além da 3.16:

| Commit | O que faz | Situação aqui |
| --- | --- | --- |
| `226162edf` (snowstormxd, de selevet-eglantine) | Títulos nativos submetem à GPU a cada 128 draws em vez de 4096 | Portado atrás de `EDEN_FLUSH_DRAWS=N` (padrão 4096): deve encurtar o `Finish` por frame, que espera tudo que ainda não foi submetido |
| `202c6b035` | Reaproveita a alocação de um uniform buffer pequeno com os mesmos bytes no mesmo command buffer | Não portado: troca uma cópia de até ~4 KB por uma comparação de custo parecido |
| `46b2577bf` | Retornos antecipados em laços de buffers com máscara zero | Não portado: ganho desprezível |
| `92d49d8a0`, `d125cfb41` (Duck912) | Substituto temporal do DLSS do FE (jitter + vetores de movimento, histórico reprojetado) | Qualidade de imagem e GPU, não a thread de trás; o nosso fork não tem. Candidato à parte |
| `18b0671e1`, `808feaf3b` (Duck912) | Correções de renderização do FE no recompilador do PokeDinner | O nosso fork já renderiza o FE por outro caminho |
| `690f80e15`, `0f14696044` | Pacing do vi2 deles (frame atual/anterior) | Ver 3.16: não se transporta |

### 3.18 Revisão de correção do branch (2026-11-04)

Revisão do diff `origin/main...perf/back-thread-split`, apontamentos conferidos no código:

| Apontamento | Correção |
| --- | --- |
| `EDEN_INLINE_DEFER` adiava também a invalidação do cache de texturas; uma leitura da CPU que baixa uma imagem escrita pela GPU (`FlushRegion`), o `DrawTexture` (sem `FlushCaching`) ou o present podiam ver/devolver a imagem velha | Modo `1` agora adia só buffer e shader (seguro: a faixa não tem dados da GPU para baixar, e todo bind e busca de pipeline vem depois de um `FlushCaching`); o comportamento do PokeDinner ficou no modo `2`, com o risco documentado. `DrawTexture` passou a chamar `FlushCaching` como os outros draws |
| `EDEN_CPU_WRITE_PAGES`: uma página lembrada nunca saía da lista | As páginas lembradas valem só até as escritas seguradas do núcleo serem aplicadas (`GPUDirtyMemoryManager::Gather`, a cada command list ou fence: uma época por núcleo); depois são consultadas de novo |
| (achado ao conferir) A leitura da CPU que baixa dados da GPU sobre a memória do guest (`gpu.cpp`, `GetFlushArea` → `FlushRegion`) não aplicava antes as escritas seguradas: o download podia sobrescrevê-las. Já existia antes deste branch | `InvalidateGPUCache()` antes do `FlushRegion`, como o PokeDinner faz; o lambda copia a área em vez de referenciá-la |
| `EDEN_FLUSH_DRAWS` dava período de N+32 (160 para 128) | Arredonda N para múltiplo de 32 e submete a cada N−1 draws; sem a variável, o comportamento é o de antes |
| `is_draw_thread` ficava ligado na frente se ela preparou draws antes do split começar (estatística errada e corrida ao ler o `chunk` do scheduler) | Zerado na frente quando o split começa |
| Carimbos de "acordar" deixados por uma espera que não dormiu eram usados pela próxima | Zerados antes de cada tentativa de dormir |
| "front slept per drain" contava também as esperas por espaço no ring | Conta só as do `Drain` |
| `acquire_changed` comparava a leitura crua com uma que pode vir de um readback de buffer da GPU; operações desconhecidas contavam como máscara | Compara com outra leitura crua; só as três operações conhecidas são contadas |
| Saídas antes de pegar o lock caíam em `unlock` | Caem em `tail` |
| Tabela de downloads repetida por especialização do `ConfigureImpl` | Uma só, numa função fora do template |
| `Begin()` sem uso | Removido |

Também conferido: a invalidação do shader cache em lote equivale à por região (uma entrada marcada sai do
`invalidation_cache` na hora e `RemovePendingShaders` remove duplicatas); `StatsQueued` pega só o
`queue_mutex`, sem ordem nova de locks; as anotações de release lidas pela frente só depois de um `Drain` (thread de
trás parada, `consumed` com release/acquire).

### 3.19 Rodada 11 (praça, `fa0ef24983`)

- **`EDEN_CPU_WRITE_PAGES=16` rejeitado:** o contador de FPS não muda, mas o jogo "parece 10 FPS" (usuário).
  Provável causa: escritas do jogo seguradas até a próxima command list chegam aos caches da GPU uma lista
  atrasadas (dados de animação/constantes de um frame no seguinte) e o ritmo das apresentações fica irregular
  (39-64 ms entre `QueueNativeImage`). Fica desligado; não vira padrão.
- `EDEN_FLUSH_DRAWS=128` (com PAGES=16): `Finish` 127 → 62 ms/s (2,2 → 1,0 ms por download), `handles` 21,1 →
  15,3%, µs por draw 8,67 → 7,86, draws/s 73,4 → 79,4 mil (+8%). Com `EDEN_INLINE_DEFER=1` junto: upload inline
  4,1 → 2,3%, 7,79 µs por draw, 82 mil draws/s. Precisa ser repetido **sem** PAGES=16.
- Decomposição: escritas de macro 80% do tempo do ring (3,5-3,9 mi/s; os draws rodam dentro delas); trabalho
  próprio da engine ~19-20%; **aplicar as escritas seguradas do jogo no início de cada lista: 7,6-7,8%**; fim de
  lista 0,2%.
- Tabela de handles: ~60 downloads/s, 3 draws depois do último dispatch, **bytes iguais ao download anterior em
  100%**. `Build`: 47-48% dos elementos mudam (incremental não compensa). `IsRegionGpuModified`: ~850 mil/s, 13%
  verdadeiro, 100% dentro de uma página. Descriptor sets religados com as mesmas entradas: 0 (reaproveitar sets
  não serve). Sinais coalescidos: todos na hora (nenhum adiado para fence).

### 3.20 Rodada 12 e o que vira padrão

Praça, sem `EDEN_CPU_WRITE_PAGES`, só a estatística ligada:

| | referência | `FLUSH_DRAWS=128` | + `INLINE_DEFER=1` |
| --- | --- | --- | --- |
| Draws/s | 74,1 mil | 75,0 mil | 73,9 mil |
| Ocupada (% por mil draws/s) | 1,221 | 1,199 (−1,8%) | 1,200 |
| `Finish` por frame | 45 ms/s, 760 µs | 15 ms/s, 255 µs | 12 ms/s, 208 µs |
| Upload inline / `flush caching` | 4,4 / 7,9% | 4,0 / 8,1% | 2,2 / 8,2% |

Fluido nas três (usuário). **Padrão agora:** envio à GPU a cada 128 draws nos títulos do Switch 2 (desktop);
`EDEN_INLINE_DEFER` fica desligado (o custo só mudou de lugar); `EDEN_CPU_WRITE_PAGES` desligado (colateral).
Escritas de registrador redundantes: o `FrontWrite` já descarta as de mesmo valor em registradores sem efeito
colateral antes de mandá-las à thread de trás (`maxwell_3d.cpp:849-855`), equivalente ao `15eef40dd1` do
PokeDinner no caminho que pesa (as escritas de macro); o restante (~700 mil métodos/s) é pequeno. O Eden
removeu o retorno antecipado geral de `ProcessDirtyRegisters` de propósito (`af554c0baa`, regressões em jogos do
Switch 1). Rodada 13: tamanho dos lotes de invalidação (regiões, regiões depois de juntar as contíguas, páginas
distintas) nas escritas seguradas (~7,5%) e no `flush caching` (~8%).

### 3.21 Rodada 13: lotes de invalidação

- Escritas seguradas do jogo (~7,5% da thread de trás no início de cada lista): 332 lotes/s, **401 regiões por
  lote, 37,8 depois de juntar as contíguas**, 14 páginas distintas. Cada região passava sozinha pelos caches de
  textura, buffer e shader.
- `flush caching`: 59 mil lotes/s, 3,7 regiões por lote, 2,3 depois de juntar: o custo é por lote (locks), juntar
  quase não muda.
- Feito (padrão, `EDEN_HELD_WRITE_MERGE=0` desliga): `GPU::InvalidateGPUCache` ordena o lote e junta as regiões
  contíguas ou sobrepostas **dentro da mesma página de 4 KB** (união exata, os mesmos bytes; dentro da página,
  uma região nunca atravessa dois buffers do buffer cache, criados em fronteiras de página). Teste fora da árvore:
  20 mil lotes aleatórios (até 600 regiões em blocos de 64 B), mesma cobertura e nenhuma região atravessando
  página, com ASan/UBSan.

### 3.22 Rodada 14 (`0923d60dac`)

- Pior ponto da praça: aplicar as escritas seguradas no início de cada lista caiu de ~7,5% para **1,2%** da
  thread de trás (fim de lista 0,3%); resíduo da engine 13,1%.
- Andando pela cidade (provavelmente a 30 FPS): início de lista 2,7%.
- Usuário: **um trecho da praça que ficava em 24 FPS agora fica em 30.**

## 4. Fase 1a: identidade do payload, sem cópia nem comparação nos acertos

Só no caminho de descriptor buffer do **graphics**, que é onde ninguém lê o payload quando a alocação é
reaproveitada. Quem autoriza buracos é um parâmetro explícito por draw (`CompactTextureBinding::allow_holes`,
ligado só por `GraphicsPipeline::ConfigureImpl` quando `uses_descriptor_buffer` e o modo 1a estão ativos),
nunca o modo da fila (`UpdateDescriptorQueue::UsesDescriptorBuffer` também é verdadeiro para o compute). Compute,
push descriptor e descriptor set sempre recebem o payload completo.

### 4.1 Ids de conteúdo

- `DescriptorCache` (`compact_texture_table.h:53`) ganha `u64 content_id`. Um contador global
  `u64 next_descriptor_content_id` (só a thread de trás escreve; nenhuma atômica) dá um id novo a **toda**
  mutação de `words`: o caminho de falha (`resize` + `memcpy`, `pipeline_helper.h:~505-520`) e o de patch
  (`PatchCompactElements`, `:450-480`). Para não esquecer nenhum lugar, `words` passa a ser escrito só por dois
  métodos (`Rewrite(...)` e `PatchElement(...)`) que trocam o id. Um `u64` não dá a volta, então não há ABA,
  mesmo se a tabela for liberada e outra nascer no mesmo endereço.
- Invariante: **mesmo `content_id` ⇒ mesmos bytes em `words`.** Ids diferentes não dizem nada (o conteúdo
  pode ser igual).

### 4.2 Por draw

Duas listas por draw, num `thread_local` da thread de trás, **zeradas no início de cada `ConfigureImpl`**
(não no fim: o `ConfigureImpl` pode sair antes de `ConfigureDraw` por `return false`, `vk_graphics_pipeline.cpp:992-995`,
ou por exceção, e uma lista suja do draw anterior não pode sobreviver):
- **blocos**: todo bloco compacto deste draw, copiado ou não, com `{offset, count, content_id}`;
- **buracos**: os blocos que não foram copiados, com `{offset, count, words}`.

`offset` é relativo ao `upload_start` do draw (`UpdateData()`), não ao slice nem ao `payload_start`. Os blocos
são registrados na ordem do payload, então ficam em offsets crescentes. Na saída de `PushImageDescriptors`
(modo verify: sempre), um assert confere ordem estrita, nenhuma sobreposição e `offset + count ≤
num_descriptor_entries`.

- No acerto (`same_banks && !patched`) e com `allow_holes`: em vez de `AddEntries`,
  `guest_descriptor_queue.Skip(desc.count)` (avança o cursor sem escrever), registra o bloco e o buraco. Na
  patch, na falha e sem `allow_holes`: escreve no payload como hoje e registra só o bloco (com o
  `content_id` que acabou de ganhar).
- **Estabilidade de `words.data()`**: entre o registro do buraco e a materialização (passo 2 abaixo, no mesmo
  `ConfigureImpl`), nada mexe em `words` daquela tabela: o mesmo descritor não aparece duas vezes no draw, e o
  único escritor é o `PushImageDescriptors` do próprio bloco. Mesmo assim, o buraco guarda o `content_id`, e a
  materialização confere `cache.content_id == hole.content_id` (assert no verify). **O ponteiro `words` nunca
  vai para o worker**: os buracos são preenchidos antes do `Record` (seção 5).
- `ConfigureDraw`, caminho de descriptor buffer:
  1. **Chave rápida:** mesma geração do ring (como hoje) **e** lista de blocos `(offset, count, content_id)`
     igual à da referência **e** `memcmp` só dos trechos fora dos blocos (buffers e elementos avulsos, poucos
     KB) contra `last_descriptor_payload`. Se tudo bate: reaproveita a alocação. Nada copiado. (Os trechos
     **dentro** dos blocos não são comparados nem quando foram copiados: o id igual já garante bytes iguais.)
  2. Se não bate: preenche os buracos (`memcpy` de `words`) e faz o caminho de hoje: `memcmp` completo, e então
     reaproveita ou aloca, escreve e `assign`.
  3. **A referência é atualizada sempre que o passo 2 termina**, reaproveitando ou não: a lista de blocos do
     draw atual passa a ser a da referência. `last_descriptor_payload` (os bytes completos) só é reescrito quando
     aloca, como hoje. Quando o `memcmp` completo confirmou igualdade, os bytes da referência já são os do draw
     atual, então guardar os ids novos com eles é consistente. Sem isso, um draw que primeiro falha (sem
     blocos registrados) e depois acerta com o mesmo conteúdo nunca aprenderia a chave rápida e repetiria o
     fallback para sempre.
  4. Invariante da referência: **para cada bloco da lista de referência, os bytes de `last_descriptor_payload`
     naquele trecho são os de `words` com aquele `content_id`.** Vale depois de alocar (escritos dali) e
     depois de um fallback igual (comparados byte a byte).
- Custo: no acerto, ~0 em vez de ~150 KB de tráfego por draw com banco. Na falha, o de hoje mais o
  preenchimento dos buracos (a mesma cópia que `AddEntries` faria).

### 4.3 Riscos e cuidados

- **Payload com buracos é lixo para qualquer leitor.** A regra é sobre **consumo**, não sobre o `Record`:
  - `write_descriptors = true` (alocação nova): o payload é materializado por inteiro antes de publicar o
    comando que o lê;
  - reaproveitamento (acerto rápido): o payload fica com buracos, e nenhum consumidor o lê; o comando só usa o
    offset da alocação pronta;
  - verify: materialização obrigatória para hash e comparação.
  Asserts no verify: compute, push e set nunca têm buracos; todo comando com `write_descriptors = true` sai
  com o payload completo. Proibir buracos em todo `Record` no modo normal devolveria a cópia que 1a elimina.
- `rescaling.PushTexture(cache.rescaled)` continua no acerto (não depende da cópia).
- `Skip` precisa da mesma checagem de capacidade de `Acquire` (o `reserve` já cobre `num_descriptor_entries`).
- **Lixo no slice e verify:** um buraco deixa no slice os bytes de um frame anterior. Zerar não deixa esses
  bytes equivalentes aos descritores omitidos, só determinísticos. Por isso, no verify, os buracos são
  **sempre materializados** antes de qualquer hash ou comparação (custo do caminho de hoje), e o hash de H2 é
  calculado só sobre o payload completo.

## 5. Fase 1b: escrita do descriptor buffer no worker

### 5.1 O que muda

Em `ConfigureDraw` (`vk_graphics_pipeline.cpp:1012-1029`), quando há alocação nova: a thread de trás faz
`Allocate`, preenche os buracos (1a), decide e guarda o estado de reaproveitamento, e **não** chama
`WriteDescriptorBuffer`. O lambda de draw que já é gravado (`:1065`) ganha três capturas: `entries`
(ponteiro para o slice do payload, já completo), `alloc.host` e `bool write_descriptors`. O lambda continua
pequeno (dezenas de bytes; o limite do chunk é 32 KB).

**A escrita é a primeira instrução do lambda**, antes de qualquer `return`. Hoje o lambda sai cedo se
`bind_pipeline && !pipeline` (`vk_graphics_pipeline.cpp:1076`); se a escrita ficasse depois, a thread de
trás já teria publicado a alocação como reaproveitável (`last_descriptor_buffer_*`) e o draw seguinte usaria
memória nunca escrita. Regra geral: **uma alocação só vira referência de reaproveitamento se o comando que a
escreve for executado incondicionalmente antes de qualquer comando que a use.** Os dois estão no mesmo fluxo
FIFO, e a escrita não depende de nenhuma condição do worker. Alternativa equivalente: gravar a escrita num
`Record` próprio, logo antes do lambda do draw.

Compute (`vk_compute_pipeline.cpp:292`) fica fora no início (poucos dispatches); o mesmo padrão serve depois.

### 5.2 Por que é seguro (tempo de vida e ordem)

- `entries`: o slice do payload só é reescrito (a) depois de 8 `TickFrame` (`FRAMES_IN_FLIGHT`), sem espera
  nenhuma no próprio `TickFrame` (`vk_update_descriptor.cpp:32-37`), ou (b) num estouro, que faz `WaitWorker`
  antes de reiniciar o cursor ou realocar. O caso (b) é garantido por `WaitWorker`: ele despacha, espera a fila
  esvaziar e então pega o `execution_mutex`, que o worker adquire antes de soltar o `queue_mutex`
  (`vk_scheduler.cpp:75-85`, `:313`). Logo, quando `WaitWorker` volta, nenhum chunk está em execução nem na
  fila. O caso (a) depende do present, demonstrado abaixo. É o mesmo contrato em que o push descriptor e o
  `UpdateDescriptorSet` já confiam hoje. Com 1a, quando o comando vai escrever descriptores, os buracos são
  preenchidos **antes** do `Record`; no reaproveitamento, o worker não lê o payload. Em nenhum caso o worker lê
  `words` (que pode mudar no draw seguinte).

  **Demonstração do caso (a).** Sejam N o número de frames do pool do present (`image_count ≤
  MAX_FRAMES_IN_FLIGHT = 7`, `vk_present_manager.cpp:25`, `:363-375`) e c o número do composite.
  1. Cada composite pega pelo menos um frame do pool (`GetRenderFrame`, `renderer_vulkan.cpp:230`); com LSFG,
     pega mais um por frame gerado, do mesmo pool (`:243-250`). O `rasterizer.TickFrame()` roda uma vez por
     composite, no fim (`:262`), depois do `GetRenderFrame`. Os retornos antecipados de `Composite` (swapchain
     sem formato, janela oculta, `:215-227`) saem antes do `TickFrame`: as fatias não avançam sem adquirir um
     frame.
  2. Um frame só volta ao pool depois de o comando de present do composite que o usou ser **executado pelo
     worker**. Com present assíncrono, `Present` grava um lambda que o worker executa para pôr o frame na fila
     de apresentação; a thread de present apresenta e devolve. Sem present assíncrono, `Present` faz
     `WaitWorker` (`vk_present_manager.cpp:189-200`).
  3. No `GetRenderFrame` do composite c, há no máximo N frames fora do pool, todos de composites anteriores.
     Para um voltar, o worker tem de ter executado o present de pelo menos um composite j ≥ c − N.
     Os frames voltam na ordem em que foram apresentados: o worker executa os presents em ordem, e a
     `PresentThread` retira com `pop_front`, apresenta e só então devolve ao pool
     (`vk_present_manager.cpp:330-353`). Logo o primeiro a voltar é o do composite mais antigo, e o worker
     executou tudo até o present do composite c − N, inclusive.
  4. Depois do `TickFrame` do composite c, a fatia em uso é a que serviu aos draws gravados entre os
     `TickFrame` dos composites c − 8 e c − 7. Todos esses draws estão no fluxo **antes** do present do
     composite c − 7.
  5. Com N ≤ 7: c − N ≥ c − 7, então o worker já executou esses draws. **Fecha com margem zero**: N = 7 é
     exatamente o limite (o comentário em `SetImageCount` diz o mesmo). Com LSFG, cada composite gasta mais de
     um frame, então N frames cobrem menos composites e a conta fica mais folgada.
  6. O mesmo argumento vale para as views no `sentenced_image_view` (8 `TickFrame`): uma view sentenciada
     durante o frame c − 8 não é referenciada por comandos gravados depois disso, e todos os que a referenciam
     estão antes do present do composite c − 7.
  Consequências: 1b não muda essa conta (o worker continua sendo o único leitor e consome na mesma ordem).
  Um estágio novo (1c) **não quebra a conta por si só**: se for anterior ao worker no mesmo fluxo ordenado, o
  worker só executa o present depois de todo o preparo que vem antes dele, e a devolução do frame continua
  implicando esse preparo concluído. Mais latência não adianta composites, porque o pool continua limitando o
  produtor. A conta quebra se um preparo puder ficar pendente enquanto o present o ultrapassa, ou se outro
  consumidor continuar lendo o payload depois da devolução. **1c exige demonstrar que nenhum present ultrapassa
  preparo pendente e que `WaitWorker` inclui todos os estágios.** O contador de atraso em frames (3.4) serve
  para diagnóstico; ele não prova nada.
- `alloc.host`: o ring só reescreve a memória de um frame depois de `scheduler.Wait(frame_ticks[frame])`, um
  tick da GPU que só chega depois de o worker executar o lambda e submeter. Na volta dentro do mesmo frame,
  `Finish` espera a GPU, o que também implica o worker. A memória é host-coherent (o ring recusa a que não é):
  a escrita do worker antes do `vkQueueSubmit`, na mesma thread, fica visível sem flush.
- `descriptor_buffer_layout` e `device`: imutáveis depois da construção do pipeline. O pipeline vive enquanto o
  cache de pipelines vive (o lambda já captura `this` hoje).
- Handles de view e sampler dentro das entradas: `vkGetDescriptorEXT` passa a lê-los mais tarde, no worker. O
  risco é a view ser destruída entre o draw e a execução. A demonstração acima (item 6) cobre isso. O push
  descriptor já lê esses handles no worker hoje, então não há classe nova de risco, só mais draws expostos a
  ela.
- Reaproveitamento: a thread de trás pode decidir que o draw N+1 reaproveita a alocação X antes de o worker
  escrever X para o draw N. Não há problema: o lambda de N vem antes do de N+1 no fluxo, e a GPU só lê depois
  da submissão.
- Ordem com fences, queries, `Drain`, present e `TickFrame`: nada muda. Tudo que a GPU vê continua no mesmo
  fluxo de chunks, na mesma ordem. `Drain` continua garantindo só o que garante hoje (a thread de trás
  executou), e as leituras da CPU continuam esperando ticks da GPU, que já implicam o worker.
- Erros: `vkGetDescriptorEXT` é `void` e não lança. A falha de alocação continua decidida na thread de trás
  (`return false`, draw pulado, `submitted` como hoje).
- `vkGetDescriptorEXT` não exige sincronização externa (só lê os parâmetros e escreve no destino), e pode ser
  chamado de qualquer thread.

### 5.3 Extensão condicional (push e set com banco)

Só se a fase 0 mostrar draws com banco nesses caminhos. Eles precisam do payload contíguo no worker. Para tirar
a cópia da thread de trás, `words` teria de virar um snapshot imutável (copy-on-write: cada mutação aloca um
bloco novo numa arena por frame, liberada junto com o slice). É mais complexo e fica de fora até haver número.

## 6. Item L (condicional): estreitar o lock de `PrepareDraw`

Só se a espera da thread de trás pelo lock for ≥ 3%. Hoje o lock (`vk_rasterizer.cpp:278`) cobre
`Configure`, `UpdateDynamicStates`, queries, transform feedback, `draw_func` e `FlushStorageWrites`.
- Candidato: liberar o lock depois de `Configure` e do `PostCopyBarrier`, guardando antes o
  `texture_cache.IsRescaling()` (usado em viewport e scissor, `vk_rasterizer.cpp:~1351` e `:~1398`), para
  `Draw` não indireto. `DrawIndirect` e `DrawTexture` ficam como estão (usam o buffer cache no `draw_func`).
- A conferir antes: `query_cache.NotifySegment`, `CounterEnable` e `HandleTransformFeedback` pegam
  `buffer_cache.mutex` por dentro em alguns caminhos (`vk_query_cache.cpp:1410`, `:1444`, `:1501`, `:1641`);
  `FlushStorageWrites` mexe no runtime do buffer cache. Cada um fica dentro do lock ou ganha o seu.
- Semântica: entre o `Configure` e a gravação do draw, uma escrita da CPU do jogo pode marcar uma região já
  enviada para este draw. É equivalente à escrita chegar logo depois do draw (as duas coisas já são
  concorrentes no console). Os buffers não são destruídos na hora (destruição atrasada). Mesmo assim é uma
  mudança de semântica: revisar com cuidado e deixar atrás do interruptor.
- Ganho direto: só o tempo que a thread de trás espera. O ganho indireto (threads do jogo esperando menos) não
  aparece em draws/s se a thread de trás for o limite.

## 7. Sincronização, tempo e locks: invariantes e riscos

| # | Risco | O que garante | Como se verifica |
| --- | --- | --- | --- |
| H1 | Dois produtores no scheduler ao mesmo tempo (corrupção silenciosa de `chunk`, `state`, `deferred_clear`) | R1: um produtor por vez; a troca acontece só em `MaybeStart`/`Stop` do split, depois de `Drain` e `join` | Verify: o scheduler guarda o produtor **da época atual** (`std::atomic<std::thread::id>`). `GpuSplit` abre uma época nova (`Scheduler::SetProducer(this_thread)` na thread que assume, depois do `join`/`Drain`). Cada chamada de produtor confere a época e loga uma vez se divergir. Não pode ser "o primeiro produtor para sempre": o split para e volta no meio do jogo (`gpu_split.cpp:59-86`). |
| H2 | O worker lê memória já reescrita (payload, descriptor buffer) | 5.2 (demonstração do caso (a), `WaitWorker` no caso (b)); buracos preenchidos antes do `Record` | O que **garante** é o protocolo (5.2) e o harness (8.1), que exercita o protocolo, não só o hash. O hash no verify só **detecta** corrupção em campo: a thread de trás guarda o CityHash64 do payload **completo** (no verify os buracos são sempre materializados) e o worker recalcula antes de escrever. |
| H3 | Handle de view ou sampler destruído antes do `vkGetDescriptorEXT` | Demonstração 5.2, item 6: N ≤ 7 frames no pool contra 8 `TickFrame` de `sentenced_*`, margem zero | O contador de atraso em frames (3.4) só diagnostica. Mais frames no pool invalidam a conta; um estágio novo (1c) exige refazer a demonstração (nenhum present ultrapassa preparo pendente; `WaitWorker` inclui todos os estágios). Validation layers num teste curto, se o usuário topar. |
| H4 | Decisão de reaproveitamento errada (1a): id igual com conteúdo diferente | `words` só muda por `Rewrite`/`PatchElement`, que trocam o id; contador `u64` sem volta | Verify: em todo acerto da chave rápida, preenche os buracos e faz o `memcmp` completo mesmo assim; loga se der diferente. Teste diferencial (8.2). |
| H5 | Locks: espera nova ou ciclo | O worker não pega lock de cache (R2); 1a e 1b não criam lock nenhum nem mudam aquisições. Não existe uma ordem fixa `buffer → texture` a preservar: `PrepareDraw` usa `std::scoped_lock` com os dois mutexes (`std::lock`, com recuo), e os dois são recursivos, com aquisições aninhadas por dentro (query cache, `AccelerateDMA`) | Revisão de código; contadores de espera dos dois lados (3.4) antes e depois. O item L (seção 6) tem de mapear as aquisições aninhadas e os callbacks concretos de cada chamada que sair do lock, sem presumir ordem. |
| H6 | Ordem de memória entre a thread de trás e o worker | O push e o pop do chunk sob `queue_mutex` (`DispatchWork`/`WorkerThread`) criam happens-before para tudo que a thread de trás escreveu antes do `Record` | TSan no harness (8.1), que usa a mesma fila. |
| H7 | Ordem com leituras da CPU, fences e queries | Inalterada: mesmo fluxo de chunks; fences por tick da GPU | Os pontos fixos de teste (seção 10) e `EDEN_DRAW_EMIT=0` como referência. |
| H8 | **Pontos seriais devolvem o trabalho movido**: `Finish` (uma vez por frame no `DownloadMemory`, mais as do texture cache e do ring), `WaitWorker` (estouro do payload, present sem thread de present) e `Wait(tick)` esperam a fila do worker, que agora tem mais trabalho. Vários deles acontecem **com os locks dos caches seguros** (`buffer_cache.h:2154` sob o lock do `PrepareDraw`), então as threads do jogo esperam junto | Nenhuma garantia de tamanho: `DispatchWork` só empilha chunks, não há controle de profundidade (`vk_scheduler.cpp:88-102`). O custo devolvido é todo o trabalho do worker ainda na fila nesse instante, que pode ser muitos chunks numa rajada de bancos novos | Medir (3.4) a fila e o tempo em cada ponto de sincronização. Dois casos: (i) worker ocioso com trabalho retido no produtor → despachar antes de um `Finish` conhecido ajuda; (ii) worker saturado → não ajuda, e o remédio é tirar trabalho do worker (cache de bytes, pergunta 5) ou o `Finish` do `DownloadMemory`. O ganho de 1b é avaliado pelo tempo da thread de trás **incluindo** essas esperas, não pela ocupação média do worker. |
| H9 | Latência de submissão maior | O worker faz mais por lote antes do `vkQueueSubmit` | Atraso da submissão (3.4). GPU a 67%: há folga; o jogo trava em 30 (intervalo 2), então alguns µs não importam. |
| H10 | SMT: o worker no mesmo núcleo físico da thread de trás rouba ciclos dela enquanto trabalha | Nada garante hoje (`SetCurrentThreadToPerformanceCores` não faz nada no Windows) | Experimento opcional `EDEN_GPU_AFFINITY=1` (só para medir): fixa a thread de trás e o worker em núcleos físicos diferentes (no Windows com Zen, os lógicos 2k e 2k+1 são irmãos). Se der diferença, discutir um padrão. Não ligar por padrão (as threads do jogo também disputam núcleo). |
| H11 | Crescimento do payload (`payload.assign`) com ponteiros vivos em lambdas | `WaitWorker` antes (já é assim) | Já coberto; o harness inclui crescimento. |
| H12 | Parar e reiniciar o split (`GpuSplit::Stop`, `MaybeStart` com as configurações mudando) | 1a e 1b não criam thread; o estado é por pipeline e independente do split | Teste manual: trocar a precisão da GPU no meio do jogo com o split ativo. |

Para 1c (se houver), além de refazer a demonstração de 5.2 (nenhum present ultrapassa preparo pendente), os riscos novos são outros: `WaitWorker` tem de esperar os dois estágios (as duas filas
vazias e os dois `execution_mutex`); o estágio de preparo não pode usar o `cmdbuf`; e quem marca o chunk de
submissão é o estágio final. O desenho fica para quando houver número que o justifique.

## 8. Verificação

### 8.1 Harness TSan (fora da árvore, como os do split)

Modelo com o código real onde der: a fila de chunks copiada de `Scheduler::DispatchWork`/`WorkerThread`, e
`UpdateDescriptorQueue` compilado com stubs de `Device` e `Scheduler` (`WaitWorker` do modelo). O produtor grava
draws com payloads de tamanho aleatório, buracos e `TickFrame`, força estouros e crescimento, e reaproveita
alocações de um ring de descritores falso. O modelo separa **`submitted_tick`** (anda quando o worker
"submete") de **`completed_tick`** (a "GPU" falsa, numa thread própria, conclui ticks submetidos com atraso
aleatório e independente do worker). O `Wait(tick)` do modelo espera `completed_tick`, como o real. O worker
confere o hash de cada payload e escreve no ring falso. Dois verificadores, separados:
- **payload**: nenhuma fatia é reescrita antes da última leitura do worker que a referencia;
- **descriptor buffer**: nenhuma região do ring é reescrita antes de `completed_tick` passar o tick da última
  submissão que a usa (a "GPU" pode estar lendo depois da submissão). Rodadas com `sleep` aleatório nos dois lados, TSan e `-O2`, ~1 milhão de draws.
O modelo inclui também o **pool de frames do present** (N configurável até 7, devolução só depois de o worker
executar o comando de present, `TickFrame` uma vez por composite e frames gerados do LSFG). O verificador
confere diretamente o protocolo: nenhuma fatia é reescrita enquanto um chunk que a referencia ainda não
executou, e nenhuma "view" sentenciada é destruída antes disso. Isso é o que prova 5.2 no modelo; o hash só
confere os dados. Testes negativos com **escalonamento determinístico** (não aleatório, que pode
não falhar): N = 8 com o worker travado num ponto escolhido até a fatia voltar ao produtor; e um ring que
reutiliza a região no `submitted_tick` em vez do `completed_tick`. Os dois **têm** de disparar o verificador. Os buracos de 1a entram no modelo com saídas antecipadas e exceções simuladas
entre o registro e o `ConfigureDraw`.

### 8.2 Teste diferencial de 1a (ASan/UBSan)

Sequências aleatórias de acertos, patches, falhas, troca de geração de view e troca de geração do ring,
aplicadas a (a) o caminho atual (`AddEntries` + `memcmp` completo) e (b) chave rápida + fallback. Exigências:
mesma decisão de reaproveitamento em todos os passos e payload final idêntico em toda alocação nova. Usar as
funções reais de `compact_texture_table.h`/`pipeline_helper.h` onde der, como no harness da tabela compacta.

### 8.3 Compilação

Como no pré-plano: `g++ -fsyntax-only -Wall` e o `clang++` com os avisos que o MSVC trata como erro, filtrando as
linhas alteradas, em `vk_graphics_pipeline.cpp` (instancia o `ConfigureImpl` e inclui `pipeline_helper.h`),
`vk_compute_pipeline.cpp` (também inclui `pipeline_helper.h`), `vk_scheduler.cpp`, `vk_rasterizer.cpp`,
`vk_update_descriptor.cpp`, `vk_descriptor_buffer.cpp`, `gpu_split.cpp` e `vk_texture_cache_base.cpp`. Cuidados
específicos: `/we5233` (uma captura usada só dentro de `if (verify)` conta como usada; uma usada só num
`if constexpr` descartado não conta), `/we4244` (contagens `size_t` → `u32`), `/we4456` (o `t` dos carimbos
encadeados).

### 8.4 No jogo

`EDEN_DRAW_EMIT_VERIFY=1` numa rodada curta pelos pontos fixos (devagar, só para pegar log). Depois as medições
com o verify desligado.

## 9. Fase 2 (B): o que decidir e o que já se sabe

Duas formas, para escolher com os números da fase 0 (baldes 2 e 3a contra o resto):

- **B-intra (a do pré-plano):** dividir o `PrepareDraw` em leitura (registradores, handles, `Build`) e
  resolução. Armadilhas no código: o `ConfigureImpl` chama `buffer_cache.DownloadMemory` no meio da
  decodificação (`~:501`, a tabela de handles escrita pela GPU), então a leitura de um draw não pode correr na
  frente da resolução do anterior quando os dados vêm da GPU. `texture_cache.SynchronizeDescriptors` (TIC/TSC
  da memória guest) abre o `ConfigureImpl`. As flags de sujeira de `maxwell3d->dirty` são consumidas e zeradas
  por vários leitores (pipeline cache, state tracker, estados dinâmicos), então um snapshot teria de levá-las
  junto.
- **B-split (segundo split, motor | rasterizador):** a thread de trás fica com os métodos da engine e o
  `DrawManager`, e uma thread nova fica com o `RasterizerVulkan` inteiro (caches, scheduler, query cache,
  fences). A interface entre as duas são as chamadas de `RasterizerInterface` (`Draw`, `Clear`,
  `DispatchCompute`, `Query`, `SignalFence`, `FlushRegion`...) viradas itens de ring, com os registradores
  replicados por log de escritas (como as cópias da frente para as macros). Reaproveita a máquina do `GpuSplit`
  (ring, `Drain`, `Defer`, harness TSan existente) e não divide o dono do scheduler. O custo é o balanceamento:
  com ~13% fora de draws na thread de trás e ~65% no `PrepareDraw`, o teto é ~×1,5 só se o estágio do
  rasterizador não crescer com a replicação dos registradores. Tudo que hoje lê o rasterizador de volta na
  hora (DMA que lê memória escrita pela GPU, semáforos, `AccelerateDMA` com retorno `bool`) vira `Drain` do
  estágio novo. Esse levantamento é o primeiro passo de B.

Decisão: se a fase 1 der ≥ +5% e nenhuma regressão, B-split é o candidato natural, com a fase 0 dizendo o
balanço. Se B não fechar no papel, C (heap bindless) volta a ser a alternativa.

## 10. Medição, aceite e entrega

- Métrica: draws/s no mesmo ponto da praça depois de estabilizar; média de 3 janelas de 4 s; o mesmo exe com
  `EDEN_DRAW_EMIT=0` e `=2`, e estatística de tempo desligada.
- Não pode piorar: palácio de Megaira, seleção de personagem, campo (~30) e uma batalha. Nem os tempos de
  `Finish` e `WaitWorker` (H8) a ponto de anular o ganho.
- Aceite para virar padrão: ≥ +3% de draws/s na praça, nada visual com o verify ligado, e o worker ocupado
  ≤ 50%. Abaixo disso o código fica atrás do interruptor, desligado.
- Branch: `perf/back-thread-split` a partir do `main`. Commits (em inglês, explicando o porquê):
  1. `video_core: time the GPU back thread by stage (EDEN_BACK_THREAD_STATS)`: fase 0.
  2. Rodada do usuário; a tabela da seção 3.5 entra no `docs/perf-handoff.md`.
  3. `vk_graphics_pipeline: reuse descriptor buffer allocations by content id` (1a) + verify.
  4. `vk_graphics_pipeline: write descriptor buffer entries on the Vulkan worker` (1b) + verify.
  5. Condicionais: L, 1c, 5.3.

## 11. Perguntas da revisão 1 e respostas

| Pergunta | Resposta (revisão adversarial, conferida no código) |
| --- | --- |
| 1. Outros consumidores do payload? | Sim: o **compute**, que lê o payload e escreve o descriptor buffer na hora (`vk_compute_pipeline.cpp:282-292`). O graphics também lê no produtor (comparação e cópia). Daí `allow_holes` explícito (seção 4). |
| 2. Descriptor buffer na RTX? | Provável para os bancos grandes, não demonstrado. `CanUseDescriptorBuffer` exige a extensão, bindings e `combinedImageSamplerDescriptorSingleArray` (`pipeline_helper.h:156-171`), e o graphics ainda exige ring válido e capacidade (`vk_graphics_pipeline.cpp:296-305`). A fase 0 responde (3.3). |
| 3. Esperas dentro dos locks? | Sim. O `PrepareDraw` segura os dois durante o `Configure`, que chama `DownloadMemory` → `runtime.Finish()` (`buffer_cache.h:2154`); o `Allocate` do ring também pode esperar sob o lock. Mais trabalho no worker prolonga a posse e a espera das threads do jogo (H8, 3.4). |
| 4. Present assíncrono e LSFG? | O pool limita o atraso nesses caminhos também, mas a até **7** frames, não 1-3. Frames gerados consomem o mesmo pool, sem multiplicar os `TickFrame`. A demonstração está em 5.2: fecha com margem zero. |
| 5. Cache dos bytes Vulkan por bloco? | Vale medir antes de qualquer thread nova. A chave tem de incluir o `content_id`, o tipo de descritor e o tamanho/representação (stride) do layout; o destino respeita offset e stride de cada binding. Os bytes contêm handles: o cache não sobrevive ao `content_id` (que muda quando os handles mudam). Nada de presumir portabilidade entre dispositivos. |

## 12. O que mudou na revisão 2

Aceito como veio:
- Buracos proibidos fora do graphics por autorização explícita por draw (`allow_holes`), não pelo modo da fila (4).
- Justificativa de lifetime trocada por uma demonstração (5.2): pool ≤ 7, devolução de frames só depois de o
  worker executar o present, 8 fatias e 8 `TickFrame`. Margem zero; 1c exige refazer a demonstração (revisão 3). O contador de atraso virou
  só diagnóstico (H3).
- A chave rápida de 1a aprende também depois de um fallback igual: a lista de blocos de referência é
  atualizada sempre que o passo 2 termina, e os blocos são registrados copiados ou não (4.2).
- Protocolo dos buracos completo: listas zeradas no início de cada `ConfigureImpl` (saídas antecipadas e
  exceções), offsets relativos ao `upload_start`, ordem e não sobreposição verificadas, `content_id` conferido
  na materialização, `words` nunca vai ao worker, hash só sobre payload completo (4.2, 4.3, H2).
- H8: `DispatchWork` não limita a profundidade da fila; medir a fila e o retido no produtor em cada ponto de
  sincronização, com máximo por janela, para separar worker ocioso de worker saturado (3.4).
- H1: um produtor por época, com troca sincronizada pelo split; H5: sem ordem de locks presumida (7).
- Estatística: publicação por atômicos (worker, threads do jogo, contador de frame); limites do `rdtsc` sem
  serialização e conferência com `lfence` e com o perfil por amostragem (3.1).
- Ganho de 1b conta só o `WriteDescriptorBuffer` (o `assign` fica na thread de trás) e é avaliado com as
  esperas nos pontos de sincronização (3.5, H8).
- A opção A deixou de ser "inviável": é cara e fica rejeitada como primeiro passo (0, 1).

Acrescentado nesta revisão (achado ao conferir a crítica):
- O lambda de draw sai cedo (`vk_graphics_pipeline.cpp:1076`). Em 1b, a escrita do descriptor buffer é a
  primeira instrução do lambda (ou um `Record` próprio antes dele); uma alocação só vira referência de
  reaproveitamento se a escrita for incondicional e vier antes de qualquer uso (5.1).

Continua igual: a ordem das fases (0 → 1a → 1b, com 1c e L condicionais), as regras R2-R4 e a fase 2.

## 13. O que mudou na revisão 3

- 1c não quebra a garantia de lifetime por si só: exige demonstrar que nenhum present ultrapassa preparo
  pendente e que `WaitWorker` inclui todos os estágios (5.2, H3, seção 7, seção 12).
- Harness: `submitted_tick` separado de `completed_tick`, com verificadores separados para o payload (última
  leitura do worker) e para o descriptor buffer (conclusão da GPU); testes negativos determinísticos (8.1).
- Regra dos buracos reescrita por consumo: payload completo só quando o comando escreve descritores; no
  reaproveitamento ninguém lê; no verify sempre materializado (4.3, 5.2).
- 3.4: o atraso em frames é só diagnóstico e mede a idade do comando mais antigo do chunk, não do despacho.
- 5.2: a ordem FIFO deixou de ser hipótese (`PresentThread`, `vk_present_manager.cpp:330-353`), e os retornos
  antecipados de `Composite` saem antes do `TickFrame`.

Próximo passo: fase 0.
