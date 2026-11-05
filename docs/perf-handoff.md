# Passagem de contexto: otimização do eden-fefw (Fortune's Weave)

Documento para continuar o trabalho numa sessão nova. Escrito em 2026-11-01.

## Contexto

- Usuário: xDarkWindx (fala português, tom casual-profissional). Windows, Ryzen 7 5700X, RTX 5060 Ti 16 GB,
  96 GB RAM, monitor 120 Hz. Jogo: **Fire Emblem: Fortune's Weave** (Switch 2).
- Repositórios do usuário (ele autorizou commit e push direto; o `AGENTS.md` foi removido dos dois):
  - `xDarkWindx/eden-fefw`: fork do `Duck912/eden-fefw` (git.eden-emu.dev). Branch de trabalho: **`main`**.
    - `upstream-main`: cópia do main do Duck912 que o usuário sobe (o servidor do Duck912 é bloqueado na
      rede da sessão). Para atualizar: o usuário roda `git fetch upstream` e
      `git push origin upstream/main:refs/heads/upstream-main`; depois a sessão faz o merge no `main`.
    - `upstream-patch`: só as 3 primeiras otimizações sobre o `2dd14bc` do Duck912, com trailer
      `Assisted-by` (regra de IA do projeto Eden), para mandar a ele. Ainda não inclui os commits seguintes.
    - `perf/reuse-bound-pipeline`: branch antigo, já incorporado ao `main`.
  - `xDarkWindx/Eglantine-fook`: fork do Eglantine (branch padrão `nx2`). Só teve o `AGENTS.md` removido.
- Commits: mensagens em inglês, explicando o porquê; terminar com as linhas `Co-Authored-By` e
  `Claude-Session` que a sessão indicar. Autor: `xDarkWindx <darkwindrox@gmail.com>`.

## Fluxo de build e teste do usuário

```powershell
git pull
cmake --build build --target yuzu
Copy-Item build\bin\eden.exe, build\bin\eden.pdb "E:\EmuRoms\Emuladores\eden-fefw-v1.05-windows-x64\" -Force
```

- Ele roda o exe dentro da pasta do v1.05 (lá está a pasta `user`, com chaves, firmware e saves).
- O título da janela só atualiza o hash quando o CMake reconfigura; não serve para saber a versão.
- Build com `-DEDEN_PROFILE_SYMBOLS=ON` já ligado (PDB completo do `video_core`, para perfis).
- Perfis: Visual Studio > Criador de Perfil de Desempenho (Alt+F2) > Processo em Execução > Uso da CPU.
  Cuidados: (1) sem o PDB completo, funções sem símbolo público aparecem com o nome do símbolo vizinho
  (`RuntimeError`, `span<vector>`, `KeplerMemory`...); (2) no código-fonte, o custo de uma chamada aparece
  na linha **seguinte** (endereço de retorno); (3) lambdas com `LAMBDA_FORCEINLINE` somam na linha da chamada.
- Variáveis de ambiente úteis: `EDEN_GPU_PROFILE=1|2` (perfil de GPU; 1 serializa), `EDEN_FINISH_TRACE=1`
  (pilhas dos `Scheduler::Finish` e de quem escreve a tabela de handles), `EDEN_PRESENT_STATS=1`,
  `EDEN_VI2_MAX_RESOLUTION_MODE=1|2|4`.

## Diagnóstico

- A tela emulada é 60 Hz fixa e o jogo usa swap interval 2: **30 FPS só se o frame couber em 33,3 ms**;
  se passar um pouco, cai para 50 ms (FPS em degraus: 30/24/22/20). VSync do PC não influi.
- O gargalo é a **thread da GPU** do emulador (preparação de draws, macros, compute). A GPU real e a CPU
  emulada ficam esperando (o jogo gira lendo `GetCNTPCT`). Resolução e AA quase não mudam o FPS.
- Fortune's Weave usa "arrays de texturas em buffer" (`buffer_source`): todo draw vincula todos os
  registros da tabela, com índices muito repetidos. Era o maior custo por draw.
- Uma vez por frame, a tabela de handles é escrita por um compute shader e baixada com `Finish`
  (`vk_graphics_pipeline.cpp`, `DownloadMemory`). O experimento de pular isso não mudou o FPS e foi revertido.

## O que foi feito (no `main`)

| Commit | O que faz | Teste |
| --- | --- | --- |
| f1c6b38 | Pipeline já resolvido passado ao `UpdateDynamicStates` (evita 3 a 5 buscas por draw) | jogo |
| 9c1e70d | Memo por draw de views (`FillImageViews`) e samplers (`lookup_memo.h`) | A/B: 22 -> 30 FPS no level-up |
| c1fdbfe | Memo de handles de sampler em `PushImageDescriptors`; views escritas no lugar | batalha 26 -> 30 |
| b81abe6 | Decodificador ASTC na GPU sem `Finish` (só `Flush`) | sem teste A/B dedicado |
| fc79ad1 | `UsageTracker`: pula faixas repetidas e limpa só páginas sujas | +2 FPS nos esquadrões; teste aleatório |
| ecf3785 | Interpretador de macros sem cópia do banco de registradores | diferencial com 200 mil programas |
| f30e550 | Samplers do `ConfigureImpl` escritos no lugar | trivial |
| ef68e0d | Opção CMake `EDEN_PROFILE_SYMBOLS` (`/Z7` só no `video_core`) | usado nos perfis |
| 111f225, 439d9f3 | Diagnóstico `EDEN_FINISH_TRACE` (`stack_trace_diagnostics.*`) | usado |
| 4e792b1 | Merge do v1.07 do Duck912 (24ccb4d, 8c6f052: crashes no cache de buffers) | compilado pelo usuário |
| 5735096 | JIT de macros portado do Eglantine (`macro_turing_jit.h`), com o DMA de FIFO na semântica do Duck912 | diferencial com 100 mil programas e 1,1 milhão de leituras de DMA; 3,9x no benchmark. Usuário compilou no MSVC e o jogo rodou sem artefatos |
| 46cc3bf | Cache de tradução do `DeviceMemoryManager` por thread (Eglantine 06f294c7; corrida com Map/Unmap) | compilado |
| 36f4c66 | Worker do Vulkan recebe trabalho a cada 32 draws e só é acordado se estiver esperando (Eglantine a99484f3) | compilado |
| **141c986** | **Thread da GPU dividida em duas** (`gpu_split.*`, Eglantine 7a251d3a e seguintes). `EDEN_GPU_SPLIT=2` (padrão): macros na frente com cópia dos registradores; `=1` macros atrás; `=0` uma thread. `EDEN_GPU_SPLIT_VERIFY=1` compara as cópias a cada 4096 macros e loga diferenças | harness do ring (4,8 mi de operações, TSan) e harness do Maxwell3D real (5 mi de chamadas, 612 mil macros); detalhes no commit. **Usuário ainda não testou no jogo.** |
| 8c82aa1 | Hash de passagem para chaves inteiras em `Common::unordered_map/set`; `RangeSet::Intersects` em `IsRegionGpuModified` (Eglantine 6b718567) | `Intersects` contra `ForEachInRange`: 6 sementes, depois 40 x ~100 mil |
| fdf816c | Tradução de shaders: busca em largura linear e cache do rastreio de storage buffers (Eglantine 0380b062, adaptado ao `Track` nativo do Duck912) | lógica do "visitado" contra a busca linear: 20 + 2000 casos |

Interruptores para A/B: `EDEN_NO_TEXTURE_DEDUPE=1`, `EDEN_ASTC_GPU_FINISH=1`, `EDEN_MME_JIT=0`.
Configuração recomendada ao usuário: ASTC Recompression = Uncompressed. `EDEN_MACRO_UPLOAD_BATCH=1`
(do Duck912) deu +1-2 FPS, mas tem um bug conhecido de tela preta acima de ~56 FPS.

Resultados: level-up, mapa e batalhas em 30 FPS (antes, 18-22). Mapa com 3 esquadrões inimigos: ~24
(antes, ~20). Nessa cena, a thread da GPU fica em ~80% de um núcleo.

O objetivo do usuário é o jogo inteiro a pelo menos 30 FPS; ele não quer medir o ganho de cada mudança
separadamente. Teste preferido: rodadas pequenas e variadas primeiro, depois a grande.

## Pendências, em ordem

1. **Testar o split (141c986) no jogo.** Ligado por padrão. Se der crash ou artefato: `EDEN_GPU_SPLIT=0`
   (uma thread) e `EDEN_GPU_SPLIT=1` (macros atrás) isolam a etapa; `EDEN_GPU_SPLIT_VERIFY=1` loga
   "Split front register" se a cópia da frente divergir. Pontos específicos do fork já tratados (ver o
   commit): draws instanciados adiados (`ProcessDirtyRegisters` emite o draw em qualquer escrita fora
   do laço) e flags de falha nativa (`native_execution_failed`, zeradas depois de cada método).
2. **Regressão: grama piscando e uma animação de braço estranha.** Apareceu depois do build com o merge
   do Duck912 e das duas mudanças pequenas. Plano: abrir sem `EDEN_MACRO_UPLOAD_BATCH` (as variáveis
   continuam valendo na mesma janela do PowerShell); testar o release oficial v1.07 do Duck912 (pasta nova,
   com junction para a pasta `user`); se o oficial estiver bom, compilar o `f30e550` (antes do merge).
   Interruptores: `EDEN_NO_TEXTURE_DEDUPE=1`, `EDEN_ASTC_GPU_FINISH=1`, `EDEN_MME_JIT=0`.
3. Itens do Eglantine ainda não trazidos (avaliados): invalidação adiada de uploads inline (1c7c2468),
   fences (1c2ed2a0, 544977c0: o Duck912 tem coalescência própria, conflitam), locks dos caches
   (e08b4025, invasivo), coletor de buffers com uma espera (4d7558e2, só ajuda com VRAM no limite).
   Depois do split, o gargalo do Eglantine passou a ser a preparação de draws na thread de trás.
4. Atualizar o `upstream-patch` com b81abe6, fc79ad1, ecf3785, f30e550, o JIT e o split.
5. Release no GitHub para o pessoal do Reddit: zip do `build\bin` com
   `robocopy build\bin <destino> /E /XJ /XD user /XF *.pdb *.ilk *.lib *.exp`. **Nunca** incluir a pasta
   `user` (chaves e firmware). Opcionalmente, GitHub Actions.
6. Ideia arriscada, não iniciada: reaproveitar conjuntos de descritores entre draws. Precisa detectar
   mudanças nas tabelas TIC/TSC e nas imagens.

## Sessão de 2026-11-02: artefatos resolvidos

Sintomas antigos (grama e capa piscando, cabeça solta, capa esticada; prédios pretos depois) resolvidos
no `main` até 2b73a41, com GPU Mode = Fast. O que resolveu, do PokeDinner `c3be47da` (fork do Eglantine,
espelhado no branch `pokedinner` deste repositório; o servidor git.eden-emu.dev é bloqueado na sessão):
- 6d535dd: escritas da CPU seguradas (memória que a GPU também escreve) são aplicadas antes de cada
  command list (`InvalidateGPUCache` no envio) e copiando só os bytes (`OnCachedWrite`). Arrumou o
  personagem e a grama. Interruptores: `EDEN_HELD_WRITES_AT_SUBMIT=0`, `EDEN_CACHED_WRITE=0`.
- 2b73a41: jogos do Switch 2 copiam de volta só as páginas que a CPU lê (`NoteCpuRead` via
  `GetFlushArea`; precisa do Reactive Flushing ligado). A cópia de tudo trazia a versão velha da GPU
  por cima do que o jogo tinha escrito. Resolveu o resto, até a grama de longe. `EDEN_BUFFER_DOWNLOADS=1`
  volta a copiar tudo.
- Tentativas descartadas (revertidas): "smart fences" (borrão, 19 FPS, um travamento) e ler command
  lists escritas pela GPU da memória (prédios pretos/invisíveis: as páginas marcadas cobrem janelas
  inteiras de storage, e a lista lida da memória já era a do frame seguinte).
- Diagnóstico útil: GPU Mode Accurate e Fence Behavior Balanced escondiam o problema (com muito custo);
  `EDEN_NX2_PB_SNAPSHOT=0` piora (rochas piscando).

Resultado (usuário, 6891933): Fence Behavior Default, GPU Mode Fast, campo aberto em ~30 FPS quase sempre,
sem artefatos. Configuração recomendada: Default. Próximos passos: medir a cena dos 3 esquadrões; grupo de compilação de shaders do
PokeDinner (Wael3rd: 9a876a74, 452dc273, b59fd995, bebdff8c e os do recompilador) para menos travadas;
atualizar o `upstream-patch` para o Duck912.

## Método de teste usado aqui

- Build Linux do `video_core` nesta sessão (só para compilar e ligar harnesses): CMake 3.31 via pip,
  dependências do apt, e as que o proxy bloqueia (downloads de arquivo do GitHub, git.eden-emu.dev,
  googlesource) clonadas com git e passadas como `-D<pacote>_CUSTOM_DIR=...`; stubs para libgav1 e tzdb;
  `-Dfmt_FORCE_BUNDLED=ON -Dxbyak_FORCE_BUNDLED=ON -DENABLE_WERROR=OFF`. Cuidado com `pkill -f <padrão>`:
  ele mata o próprio shell se o padrão estiver na linha de comando.
- Harness do Maxwell3D real: liga `maxwell_3d.o`, `draw_manager.o`, `engine_upload.o`, `macro.o`,
  `gpu_split.o` e a `libcommon.a` com `-Wl,--unresolved-symbols=ignore-all`, define no teste os métodos
  do `MemoryManager` (memória falsa) e um rasterizador que loga hash dos registradores em cada draw.

- Testes diferenciais fora do repositório, compilados com g++ e stubs (`common/logging.h`,
  `common/x64/xbyak.h` -> `<xbyak/xbyak.h>`). O Xbyak foi clonado de `github.com/herumi/xbyak`.
  O interpretador antigo vem do `git show <commit>:src/video_core/macro_turing.h`, renomeado de namespace.
- Arquivos com CRLF: `texture_cache/texture_cache.h`, entre outros. Edite preservando as quebras de linha
  (Python com `newline=''`) e confira o `git diff --stat`.

## Crash na seleção de personagem (2026-11-02)

- Sintoma: `SetTerminateResult result=0x4201 (2001-0033)` + `svcBreak` aos ~66s, na tela dos obeliscos
  (vídeo de fundo 2560x1440 por personagem, troca com L/R). Nenhum aviso antes: o `0x4201`
  (`Kernel::ResultNotImplemented`) saía calado do `native_video_decoder.cpp`.
- Suspeito principal: `Flush` só existia para H.264; com AV1 devolvia `ResultNotImplemented`. Implementado
  (decodifica as entradas pendentes, `SignalEOS` no libgav1, `PumpOutput`). Flush que falha e `ReleaseImage`
  com fence (outro caminho `0x4201`) agora logam em Warning.
- Depois disso, `Start` logo após o `Flush` dava `0xfa01` (InvalidState, o decodificador já constava como
  iniciado); a thread do vídeo morria com PANIC e o jogo congelava ~5s depois. Agora um decodificador que
  passou por `Flush` aceita `Start` de novo (semântica do MediaCodec). `Start` que falha loga em Warning.
- Depois, congelamento: após `Flush`/`Start` o jogo devolve em loop imagens que já estavam livres (`release of
  idle image`). O caminho do Duck912 dava a cada uma uma fence nova (`++next_completion_value`) sem frame
  correspondente, abrindo buracos na sequência; as fences foram a ~197 mil e nunca eram alcançadas. Agora a
  imagem devolve a fence que já tinha (e roda `PumpOutput`); um Warning a cada 4096 dessas devoluções.
- O loop de `release of idle image` continuou: o `Flush` do AV1 decodificava as entradas pendentes, que no fim do
  clipe são EOS, e entregava esses EOS depois do `Start`; o jogo achava que o clipe novo já tinha acabado e parava
  de mandar dados. Agora o `Flush` do AV1 descarta tudo (como `MediaCodec.flush()`); saídas não buscadas devolvem
  a imagem ao decodificador. O contador de FPS do Eden provavelmente não conta as apresentações nativas (VI2):
  cai a zero com o jogo rodando a 30 (não verificado).
- PokeDinner/Eglantine não têm decodificador de vídeo nativo (outra arquitetura de Switch 2); não há o que portar.
- Se continuar: filtro de log `*:Info Service.AM:Debug` e procurar `native video` antes do crash.

## Vídeo da seleção de personagem: custo por frame (2026-11-02)

- Medido no log (1440p AV1): decode 12,8 + conversão 5,4 + swizzle 4,4 + escrita 1,3 ms ≈ 24 ms por frame, tudo
  síncrono dentro da chamada IPC do jogo (`ReleaseImage`/`Submit` -> `PumpOutput`).
- Feito: conversão e swizzle juntos por faixas de 128 linhas (um bloco do block-linear é contínuo na imagem), nas
  4 threads; buffers RGBA/tiled reaproveitados entre frames (antes, ~30 MB alocados e zerados por frame). Harness
  fora do repositório: swizzle por faixas idêntico ao inteiro (9 tamanhos, 1-7 threads) e pipeline completo idêntico
  com reuso de buffers; swizzle 3,5 -> 1,2 ms aqui.
- Resultado do usuário: "bastante tempo em 27 FPS", quedas a 20/15 menos frequentes.
- Depois: decodificação AV1 numa thread própria (`DecodeWorker`), até 2 frames adiantados; a chamada do jogo só
  converte/escreve, e espera a thread apenas se o próximo frame ainda estiver em decodificação (uma imagem devolvida
  sempre recebe o frame na mesma chamada, senão o jogo poderia esperar a fence sem fazer outra chamada). `Flush`/`Stop`
  esvaziam a fila e esperam a thread ficar ociosa antes de mexer no libgav1; `Reset` faz join. H.264 inalterado.
  Verificado: `g++ -fsyntax-only -Wall` do arquivo (fmt 11 clonado) e um modelo do protocolo com TSan (300 rodadas
  aleatórias com Flush e erros; sem corrida, deadlock ou frame fora de ordem).
- Resultado do usuário com a thread de decodificação (c960d20): seleção de personagem em 30 FPS quase sempre.
- Próximos candidatos: a cópia DMA da imagem do vídeo
  no lado da GPU (`SwizzleSubrectImpl<0,4>`/`<1,4>`, ~8,5% no perfil do usuário); threads do libgav1 (4).

## Cena de diálogo (Hong Hua, campo vermelho): ~26-27 FPS e estalos no áudio (2026-11-02)

- Perfil do usuário com símbolos da Microsoft: kernel 54%; `RtlpAcquireSRWLockExclusiveContended` 19%,
  `RtlBackoff` 15%, `NtDelayExecution` 22%: disputa de trava e sono, não trabalho.
- `EDEN_GPU_SPLIT=0` ficou um pouco pior: o split não é a causa (o `Drain` só mostra a frente esperando a de trás).
- O sleep de 600 us em leituras de `IStorage` (`service.cpp`) só liga com `nvhost_nvdec` aberto (vídeo do Switch 1);
  o FE usa o decodificador nativo: descartado.
- Trazido do PokeDinner 63244330 (Takoroka): `KSpinLock` (trava do agendador emulado) vira trava futex de 3 estados
  com ~128 pausas antes de dormir, em vez de `std::mutex`. Teste de estresse (8 threads, TSan e -O2) ok.

## Cena do palácio de Megaira (~20 FPS, muitos NPCs)

- Perfil por thread: a de trás (`GpuSplit::Execute`) a 99%, com `ConfigureImpl<SimpleStorageSpec>` em 61%
  (`FillImageViews` 11,9%, `PushImageDescriptors` 10,9%, leitura de descritores 7,6%, ~30% não identificado).
  A da frente passava 75-90% em `Drain` girando com `SwitchToThread`.
- "Sync Memory Operations" desligado: sem mudança (esperado; o limite é a thread de trás).
- Feito: `Drain`/`WaitForSpace` giram um pouco e depois dormem em `consumed` (`atomic::wait`); a de trás acorda com
  `StoreConsumed` se `front_waiting`. Par de Dekker seq_cst; modelo com TSan (200 mil esperas) ok. Objetivo: tirar a
  frente do núcleo físico da de trás (SMT) e liberar CPU.
- Próximo: Chamador/Receptor do `ConfigureImpl` na thread de trás para achar os ~30% restantes.
- Linha a linha no `ConfigureImpl` (palácio): `FillImageViews` 18%, leitura dos handles (lambdas) 33%,
  `UpdateGraphicsBuffers` 8%, `views.resize` 3% (small_vector de 64 estourava e alocava no heap a cada draw).
- Versão segura feita: por pipeline, os handles e exclusões decodificados de cada array de texturas em buffer ficam
  guardados (`BufferTextureHandles`) e são reusados quando os bytes dos registros e as outras entradas (fonte, count,
  via_header_index, limites TIC/TSC) não mudaram; spans cortados (leituras fora dos bytes) nunca são reusados.
  `FillImageViews`/`GetSamplerId` continuam rodando a cada draw (a tabela de descritores pode mudar sem aviso).
  Vetores do `ConfigureImpl` viraram `thread_local` reaproveitados. `g++ -fsyntax-only -Wall` limpo (VMA e
  Vulkan-Headers clonados). A versão agressiva (pular a reconstrução dos descritores) segue não iniciada.

## Branch perf/compact-texture-tables: tabelas de texturas compactas (2026-11-02)

- Revisão do gargalo (praça da cidade, 15 FPS, CPU): a thread de trás é o limite; `ConfigureImpl` é ~52% dela.
  Draws vazios já são descartados (`ProcessDraw`, `!count`). Custo fixo por draw (validar/baixar a tabela de
  handles) é pequeno. O grosso é por registro: centenas por draw, três passagens com memo (montagem,
  `FillImageViews`, `PushImageDescriptors`; o memo deste último tem ~40 KB e não cabe no L1).
- Causa de fundo: no Switch as texturas desses arrays são bindless (o shader lê a tabela); no Vulkan do Eden
  cada draw monta um array de descritores do tamanho da tabela. Bindless de verdade = reescrever recompilador e
  cache de texturas (fora de escala).
- Feito: `video_core/texture_cache/compact_texture_table.h`. A tabela decodificada (já guardada por pipeline) vira
  pares distintos (TIC, TSC, excluído) + o par de cada registro. Por draw: samplers resolvidos por TSC distinto,
  views por TIC distinto (junto com as outras views, mesma ordem de primeiras ocorrências), descritores feitos por
  par e copiados por registro. Nada é guardado entre draws além dos handles decodificados (que já eram).
  O reaproveitamento agressivo de descritores entre draws foi descartado: depois disso pouparia pouco na thread
  de trás e arrisca view apagada (crash de driver).
- Verificação: harness fora da árvore (caminho antigo elemento a elemento x novo com as funções reais do header,
  cache de texturas falso com repetição de passada): 20 mil draws aleatórios, 14 mi descritores idênticos, mesma
  ordem de consultas; modo com colisões nas tabelas diretas idem (mesmas primeiras ocorrências). `g++
  -fsyntax-only -Wall` de vk_graphics_pipeline.cpp e vk_compute_pipeline.cpp limpos.
- Interruptores: `EDEN_NO_COMPACT_TEXTURE_TABLES=1` (caminho antigo, mesmo build, A/B);
  `EDEN_TEXTURE_TABLE_STATS=1` (log a cada 4 s: draws/s, arrays reaproveitados, registros e pares por draw).
- Achado no log do usuário: `Payload overflow` 2x por frame (`vk_update_descriptor.cpp`). A fila de descritores
  tem fatias de 0x80000 entradas por frame e o jogo escreve >1 mi; no estouro, `WaitWorker` (a thread de trás
  espera o worker do Vulkan esvaziar). Agora, logo após essa espera (nada aponta para a fila), a fatia dobra, até
  4x (~400 MB no total com 8 fatias de 24 B por entrada; antes ~100 MB). Só cresce em jogos que estouram.
- Estatísticas na praça (`EDEN_TEXTURE_TABLE_STATS=1`): ~11 mil draws/s, só ~14/s com arrays em buffer (768
  registros, 49 pares, 3 views únicas; 0% reaproveitados). O overflow sumiu com a fila maior, sem mudar o FPS.
  O log de overflow mostrava `reserve=3112`: os pipelines comuns ligam ~3100 descritores por draw, de arrays de
  textura indexados por constant buffer (não em buffer), que passavam pelo caminho elemento a elemento.
- Estendido: arrays de constant buffer com 16+ elementos são lidos em bloco, decodificados como o `read_handle`
  (teste: 23 mi handles idênticos, com e sem secundário, deslocamentos aleatórios) e reaproveitados enquanto os
  bytes não mudam, entrando na mesma tabela compacta. Fora: offsets que estouram 32 bits e a região do shim de
  syncpoints (onde `Read<u32>` não lê a memória). Decodificação no lugar (sem alocar por draw).
- Resultado (praça): cbuf arrays reaproveitados 84,3% de ~13,7 mil/s; ~4,7 mil draws/s com tabelas; por draw
  2970 elementos, 1390 pares, **1245 views únicas**. FPS inalterado. Conclusão: é um banco de texturas bindless
  (~1250 texturas distintas, o mesmo de draw a draw). A compactação só corta pela metade; o custo restante é
  resolver/preparar (`VisitImageView`/`PrepareImageView`) ~1250 views distintas por draw (~6 mi/s), que a versão
  segura refaz por definição.
- O que daria ganho: reaproveitar views e descritores entre draws com o mesmo banco, exigindo que o cache de
  texturas garanta que nenhuma imagem foi criada/apagada/reescrita pela CPU/modificada como render target (hoje é o
  `PrepareImageView` por draw que percebe isso) — risco de view apagada; ou um heap bindless persistente no Vulkan
  (recompilador + cache de texturas). Ambos grandes. Branch não vai para o main sem A/B com ganho consistente
  (`EDEN_NO_COMPACT_TEXTURE_TABLES=1` no mesmo build).
