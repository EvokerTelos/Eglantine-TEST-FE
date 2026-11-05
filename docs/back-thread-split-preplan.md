# Pré-plano: dividir o trabalho da thread de trás da GPU (Fortune's Weave)

Leia este arquivo inteiro antes de começar. Histórico mais longo: `docs/perf-handoff.md` e
`docs/handoff-aggressive.md`. Escrito em 2026-11-04, sobre o `main` em `43a7f26`.
(Plano detalhado derivado deste: `docs/back-thread-split-plan.md`.)

## Usuário, regras e fluxo

- xDarkWindx, fala português, tom casual-profissional, sem bajulação. Windows, Ryzen 7 5700X (8C/16T),
  RTX 5060 Ti 16 GB. Jogo: **Fire Emblem: Fortune's Weave** (Switch 2). Limite semanal de uso: rodadas que
  valham o build dele.
- Repositório `xDarkWindx/eden-fefw`, **sem upstream** (desvinculado do Duck912; o histórico de diagnóstico
  removido está no branch `archive/pre-cleanup`). `main` = o que ele joga e publica (releases `1.11`, `1.12`).
  Branch de trabalho atual: `perf/draw-overhead` (igual ao `main`). Para este projeto, sugerido:
  `perf/back-thread-split` a partir do `main`. Ele autoriza commit e push nos branches de trabalho; `main` só
  quando ele pedir.
- Commits: mensagens em inglês explicando o porquê; autor `xDarkWindx <darkwindrox@gmail.com>`; terminar com as
  linhas `Co-Authored-By`/`Claude-Session` indicadas pela sessão. Nunca citar identificador de modelo.
- Notas de release: a sessão escreve em `/home/user/release-notes-vX.md` (estilo das releases 1.09-1.12, em
  inglês, mesmo rodapé); ele publica. **O proxy da sessão bloqueia push de tag**: ele cria as tags.
- Build dele: `git pull`, **`cmake -S . -B build`** (sem isso o hash no título fica velho: `scm_rev.cpp` só é
  gerado na configuração), `cmake --build build --target yuzu`, copia `build\bin\eden.exe` com outro nome.
- **MSVC trata como erro**: `/we5233` (captura de lambda não usada; já quebrou um build), `/we4456-4459`
  (sombreamento), `/we4242 /we4244 /we4245 /we4254` (conversão estreitante), `/we4388 /we4389` (sinal),
  `/we4505` (função local não usada), `/we4800`. Verificar com clang antes de cada commit (abaixo).
- Verificação nesta sessão (Linux, sem build completo): `g++ -std=c++20 -fsyntax-only -Wall` dos arquivos
  alterados com fmt 11, Vulkan-Headers, VulkanMemoryAllocator, xbyak e SDL clonados no scratchpad
  (`-DFMT_HEADER_ONLY -Isrc -Iexternals -Iexternals/glad/include -Isrc/dynarmic/src`, `libboost-dev` e
  `libavcodec-dev` via apt), **e** `clang++ -fsyntax-only -Wno-everything -Wunused-lambda-capture
  -Wunused-variable -Wshadow -Wshorten-64-to-32 -Wimplicit-int-conversion -Wsign-compare
  -DARCHITECTURE_x86_64=1` filtrando só as linhas alteradas. Templates só são checados nas instanciações:
  `vk_texture_cache_base.cpp`, `vk_buffer_cache_base.cpp` (e as do OpenGL). Lógica isolável: harness fora da
  árvore com ASan/UBSan (e TSan para concorrência).
- Arquivos com CRLF (ex.: `texture_cache.h`, `memory.cpp`, vários de `core/`): preservar ao editar.

## Por que (o que os dados mostram)

- Na praça da cidade a cena é limitada por **uma thread**: `GpuSplit::BackLoop` (thread de trás da GPU
  emulada), perto de 100% de um núcleo. O processador inteiro fica em ~33% e a GPU em ~67%.
- Ela tem **vazão constante de ~72 mil draws/s** (log `EDEN_TEXTURE_TABLE_STATS=1`, linha `Texture tables`). O
  FPS é essa vazão dividida pelos draws por frame: o jogo sobe de ~2.300 para ~3.600 draws/frame nos 5 s depois
  de carregar (NPCs aparecendo), e o FPS cai de 26 para 20. **Métrica de comparação entre builds: draws/s no
  mesmo ponto da praça, depois de estabilizar** (o contador de FPS esconde ganhos de ~5%).
- As rodadas de micro-otimização (bancos de textura, descritores, cache de faixas, lotes de escritas) levaram
  a vazão de ~45-52 mil (medição antiga, que contava só uma instanciação) a 72 mil, mas as últimas renderam
  ~1-2% cada. O que sobra na thread está espalhado em pedaços de 1-8%. Um salto só vem tirando trabalho dela.

## O que a thread de trás faz por draw (perfil da praça, `eb2190f`)

`GpuSplit::Execute` 98% → `Maxwell3D::CallMethod` 78% → `DrawManager::ProcessDraw` 65% →
`RasterizerVulkan::PrepareDraw` 64%, que segura `buffer_cache.mutex` + `texture_cache.mutex` e faz:

| Parte | % da thread | Toca caches? |
| --- | --- | --- |
| `GraphicsPipeline::ConfigureImpl` (total) | ~40 | sim |
| - leitura de handles e `CompactTextureTable::Build` | ~8-10 | lê memória guest |
| - resolução de views/samplers (bancos reusados 99%) | ~2-3 | sim |
| - `PushImageDescriptors` (descritores de textura) | ~8.5 | lê `slot_image_views`, samplers |
| - `UpdateGraphicsBuffers` + `BindHostStageBuffers` | ~12 | buffer cache inteiro |
| - `ConfigureDraw` (descriptor buffer/sets, `scheduler.Record`) | ~2 | não (só a fila) |
| `pipeline_cache.CurrentGraphicsPipeline` + estados dinâmicos + queries | ~10-15 | parcialmente |
| Métodos da engine fora de draws (registradores, uploads inline, DMA) | ~13 | alguns |

O worker do Vulkan (`Scheduler::WorkerThread`) já executa a gravação de comandos em outra thread e está em ~8%.
Os mutexes dos caches são disputados com as threads de CPU do jogo (`OnCPUWrite`, ~5% das amostras), que
esperam enquanto a thread de trás prepara cada draw.

Números a confirmar na fase 0 (são somas de um perfil por amostragem, com inlining distorcendo linhas).

## Ganho esperado (Amdahl)

Se uma fração `f` do trabalho por draw sai da thread de trás e passa a rodar em paralelo, sem virar o novo
gargalo, a vazão sobe ~`1/(1-f)`:

| `f` movido | Vazão | Praça (hoje ~20 FPS) |
| --- | --- | --- |
| 15% | ×1,18 | ~23-24 |
| 25% | ×1,33 | ~26 |
| 40% | ×1,67 | ~30 (limite de 30 do jogo) |

O jogo roda a 30 FPS no console (swap interval 2), então o teto útil é ~30. GPU real a 67% em 1x: há folga.
Riscos ao ganho: o estágio novo pode virar gargalo; sincronização por draw custa (70 mil handoffs/s exigem
fila sem lock, em lotes); disputa de cache L3 e de núcleo com as threads do jogo.

## Opções, da mais contida à mais ambiciosa

### A. Estágio de "emissão" depois da resolução (recomendado como primeiro passo)

A thread de trás resolve tudo que exige os caches (ids de views/samplers, buffers, pipeline, handles Vulkan das
views e samplers) e produz um **pacote por draw** só com dados prontos: handles, offsets, estados dinâmicos,
parâmetros do draw. Uma thread nova consome os pacotes em ordem e faz a parte que não toca caches: montar o
payload de descritores (cópias de bloco, `AddSampledImage`), `ConfigureDraw` (descriptor buffer, comparação do
payload, `vkGetDescriptorEXT`) e `scheduler.Record`.
- Ganho estimado: ~10-15% (a parte de montagem/cópia de descritores e a gravação). Pequeno, mas é a base para B.
- Cuidado: hoje `PushImageDescriptors` converte id em handle (`GetImageView(id).Handle(type)`) — isso tem de
  ficar na thread de trás (ou o pacote carrega os `SampledImage` prontos do cache de descritores).

### B. Pipeline de dois estágios na preparação

Separar `PrepareDraw` em (1) leitura de estado da engine e decodificação de handles (`cbuf_handles`, `Build`,
leitura de constant buffers) e (2) resolução nos caches + emissão. Exige um **snapshot do estado necessário
por draw** (não dá para copiar os ~14 KB de registradores a cada draw: copiar só os campos usados, ou um log de
deltas de registradores como a divisão frente/trás já faz para macros).
- Ganho estimado: ~25-40%, se bem balanceado.
- Risco alto: muitos leitores de `maxwell3d->regs` espalhados pelo rasterizer e pelos caches.

### C. Heap bindless (alternativa, não threading)

Tira o `PushImageDescriptors` e parte da resolução de textura de todos os draws (o shader lê o banco como no
hardware). ~10% estimado, semanas de trabalho no recompilador. Ver `docs/handoff-aggressive.md`.

**Recomendação:** fase 0 (medir), depois A com interruptor, medir, e só então decidir B.

## Plano por fases

### Fase 0 — medir (um build, sem mudança de comportamento)
- Contadores de tempo (rdtsc/`steady_clock` acumulado por bucket, logado com `EDEN_TEXTURE_TABLE_STATS`) em:
  métodos fora de draws, `CurrentGraphicsPipeline`, `ConfigureImpl` dividido (handles/`Build`, resolução,
  `PushImageDescriptors`, buffers, `ConfigureDraw`), estados dinâmicos, queries, `scheduler.Record`.
- Medir também o tempo dentro do lock dos caches e quantos draws/s esperam o lock.
- Saída: a tabela acima com números reais e a fração `f` que cada opção move.

### Fase 1 — opção A atrás de um interruptor (`EDEN_DRAW_EMIT_THREAD=0` desliga)
- Fila SPSC sem lock, em lotes (como o ring da `gpu_split`), entre a thread de trás e a de emissão.
- `scheduler.Record` passa a ser chamado **só** pela thread de emissão (o scheduler é de produtor único).
- Toda operação que precisa da ordem (fences, queries, flush, `Drain`, present, troca de render pass,
  `FlushCommands`, `TickFrame` da fila de descritores) vira um item da mesma fila ou espera a fila esvaziar.
- Medir draws/s com e sem o interruptor.

### Fase 2 — decidir B com os números da fase 1.

## Cuidados (o que já se sabe que quebra)

- **Produtor único do scheduler**: `Scheduler::Record`/`RecordWithUploadBuffer`, `RequestRenderpass`,
  `RequestOutsideRenderPassOperationContext`, `UpdateGraphicsPipeline`, `UpdateRescaling`,
  `UpdateDescriptorBufferChunk` guardam estado do produtor; chamados de duas threads, corrompem silenciosamente.
  Mover todos para a thread de emissão ou deixar todos onde estão.
- **Fila de descritores** (`UpdateDescriptorQueue`, `vk_update_descriptor.*`): fatias por frame, `Acquire`
  por draw, `TickFrame`; o ponteiro `UpdateData()` vai para o lambda gravado. Tem de pertencer a uma thread.
  O `DescriptorBufferRing` idem.
- **Caches não são thread-safe para leitura concorrente**: `Common::SlotVector` (`slot_image_views`,
  `slot_images`, `slot_buffers`) realoca na inserção; `boost::unordered_flat_map` (`Common::unordered_map`)
  invalida ponteiros a cada inserção (o `VirtualRangeCache` devolve ponteiro para dentro dele). A thread de
  emissão não pode ler caches; só dados copiados para o pacote.
- **Tempo de vida dos handles Vulkan**: views/imagens apagadas vão para `sentenced_*` (destruídas
  `TICKS_TO_DESTROY` frames depois). Um pacote com handle de uma view apagada só é seguro se a fila nunca ficar
  mais que poucos frames atrasada: limitar a profundidade da fila e drenar em `TickFrame`.
- **Ordem com leituras da CPU**: `GpuSplit::Drain` (semáforos, DMA de FIFO, leituras de memória escrita pela
  GPU) precisa esperar também a thread de emissão. Fences (`SignalFence`, `SignalFenceCoalesced`,
  `SignalSyncPoint`) e queries (`Query`, `ResetCounter`, condicional) idem.
- **Reaproveitamento de bancos/descritores** (`texture_bank_reuse.h`, `PushImageDescriptors`): usa
  `BankGeneration`/`ViewGeneration` e os carimbos dos bancos; se a resolução e a emissão ficarem em threads
  diferentes, essas gerações precisam viajar no pacote (o que foi validado na thread de trás).
- **Escala (rescale)**: `RescalingPushConstant` e `IsRescaling` são calculados na resolução; o push constant
  vai no pacote.
- **Feedback loops**: `CheckFeedbackLoop` decide uma barreira (`BarrierFeedbackLoop`, gravada no scheduler):
  a decisão fica na resolução, a barreira vira item do pacote.
- **`std::scoped_lock{buffer_cache.mutex, texture_cache.mutex}` por draw**: com a emissão fora, o tempo dentro
  do lock cai, o que também reduz a espera das threads de CPU do jogo (`OnCPUWrite`).
- **Erros por draw**: `Configure` pode lançar `Shader::Exception` e pular o draw; `native_execution_failed` e o
  "refusing submission completion" dependem de saber, na hora, se o draw foi submetido. Manter na thread de trás.
- **Interruptor obrigatório** para A/B e para isolar bugs, como `EDEN_GPU_SPLIT`.
- **TSan**: modelar a fila e as operações de ordem num harness com TSan antes do build dele.

## Como medir e o que não pode piorar

- Pontos fixos: pior ponto da praça (vista aberta para o centro, ~20 FPS, ~72 mil draws/s), palácio de Megaira,
  seleção de personagem e campo (~30, não podem cair), uma batalha.
- Comparar draws/s e FPS com o interruptor ligado e desligado, no mesmo exe.
- Perfil por thread no VS (Uso da CPU, filtro por thread, Chamador/Receptor): `GpuSplit::BackLoop`, a thread
  nova e o worker do Vulkan.
- Qualquer textura errada, piscando, draw sumindo ou travamento: testar com o interruptor desligado e com
  `EDEN_NO_HEAP_REUSE=1` para isolar.
