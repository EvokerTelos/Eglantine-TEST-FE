# Passagem de contexto: reaproveitamento agressivo do banco de texturas (Fortune's Weave)

Leia este arquivo inteiro antes de começar. O histórico completo está em `docs/perf-handoff.md` (mesmo
diretório); aqui está só o necessário para a próxima etapa. Escrito em 2026-11-02.

## Usuário e regras

- xDarkWindx, fala português, tom casual-profissional, sem bajulação. Windows, Ryzen 7 5700X (8C/16T),
  RTX 5060 Ti 16 GB, 96 GB RAM. Jogo: **Fire Emblem: Fortune's Weave** (Switch 2). Tem limite semanal de uso:
  seja econômico, faça rodadas que valham o build dele.
- Repositório: `xDarkWindx/eden-fefw` (fork do `Duck912/eden-fefw`, em git.eden-emu.dev, bloqueado na rede da sessão).
  - `main`: o que ele joga. Releases até **v1.10** (commit `b50a518`). Não mexer no `main` sem ele pedir.
  - `perf/compact-texture-tables`: experimentos desta etapa (base para o próximo branch, ver abaixo).
- Commits: mensagens em inglês explicando o porquê; autor `xDarkWindx <darkwindrox@gmail.com>`; terminar com as
  linhas `Co-Authored-By` e `Claude-Session` que a sessão indicar. Nunca citar identificador de modelo em commits.
  Ele autoriza commit e push direto nos branches de trabalho.
- A sessão não cria releases (API bloqueada): ela escreve as notas em `/home/user/release-notes-vX.md` e ele publica.

## Fluxo de build e teste dele

```powershell
git fetch origin
git checkout <branch>        # ou git pull se já estiver nele
cmake --build build --target yuzu
Copy-Item build\bin\eden.exe "E:\EmuRoms\Emuladores\eden-fefw-v1.05-windows-x64\eden-<nome>.exe" -Force
```

- Executáveis de teste com outro nome na mesma pasta (usam a mesma pasta `user`: saves e config).
- Variáveis de ambiente na mesma janela do PowerShell em que abre o exe (`$env:VAR="1"; .\eden-x.exe`).
- Log: `user\log\eden_log.txt`. Ele sabe usar `Select-String` e o Criador de Perfil do Visual Studio
  (Uso da CPU, filtro por thread, Chamador/Receptor, símbolos da Microsoft ligados).
- Verificação nesta sessão (Linux, sem build completo): `g++ -std=c++20 -fsyntax-only -Wall` dos arquivos
  alterados com `-Isrc -Iexternals` + fmt 11, VulkanMemoryAllocator e Vulkan-Headers clonados do GitHub
  (`-DFMT_HEADER_ONLY`); harnesses fora da árvore (g++/TSan) para lógica isolável. Ele compila no MSVC.

## Onde está o gargalo (medido)

- Cenas cheias (praça da cidade 15-21 FPS, palácio ~20, multidão de NPCs) são limitadas pela **thread de trás da
  GPU emulada** (`GpuSplit::BackLoop`, 100% de um núcleo; CPU total ~22-25%, GPU real ~60%).
- `ConfigureImpl` (`vk_graphics_pipeline.cpp`) é ~52% dessa thread.
- Estatísticas (`EDEN_TEXTURE_TABLE_STATS=1` no branch `perf/compact-texture-tables`), praça:
  ~11 mil draws/s; ~4,7 mil/s ligam um **array de texturas indexado por constant buffer** com
  **2970 elementos, 1390 pares (TIC,TSC) distintos, 1245 views distintas**; esse array é **byte a byte igual ao do
  draw anterior do mesmo pipeline em 84%** dos casos. Arrays em buffer (`buffer_source`) quase não aparecem ali.
- Ou seja: o jogo usa um **banco de texturas bindless** (~1250 texturas) em quase todo draw. No Switch custa zero;
  aqui cada draw resolve ~1245 views (`TextureCache::VisitImageView` + `PrepareImageView`), resolve samplers e
  escreve ~3000 descritores. A compactação (pares distintos) só cortou pela metade e não mudou o FPS.
- O FPS cai quando a vista abre e sobe ao se aproximar de paredes: o custo é por draw, não pelo que aparece.

## Estado do branch `perf/compact-texture-tables` (base)

1. `video_core/texture_cache/compact_texture_table.h`: `CompactTextureTable` (pares distintos + par de cada
   elemento, construção com tabelas diretas), blocos por draw, `CompactViewLookups`/`ExpandViewLookups`,
   `PushCompactTextureBlock`, estatísticas.
2. `GraphicsPipeline::BufferTextureHandles` (deque, por pipeline): handles decodificados de arrays em buffer e de
   arrays de constant buffer (16+ elementos; lidos em bloco; `cbuf_handles` em `ConfigureImpl`), reusados se os
   bytes não mudam. Arrays de cbuf são decodificados no lugar.
3. `PushImageDescriptors` (`pipeline_helper.h`) com caminho compacto (`CompactTextureBinding`).
4. `vk_update_descriptor.cpp`: a fila de descritores cresce (até 4x) quando estoura, em vez de `WaitWorker` a cada
   estouro (o jogo escreve >1 mi de descritores por frame). Sem efeito no FPS, mas tira duas paradas por frame.
5. Interruptores: `EDEN_NO_COMPACT_TEXTURE_TABLES=1`, `EDEN_TEXTURE_TABLE_STATS=1`.
Verificado: harness de equivalência (20 mil draws, 14 mi descritores idênticos, ordem das consultas igual);
decodificação de cbuf contra `read_handle` (23 mi handles). O usuário testou: sem texturas erradas, FPS igual à v1.10.

## Próxima etapa: as versões agressivas (novo branch a partir de `perf/compact-texture-tables`)

Sugestão de nome: `perf/texture-heap-reuse`. Fazer em camadas, medir cada uma.

### Agressiva 1: reaproveitar o banco resolvido entre draws

Quando o array do draw tem os mesmos bytes do anterior (o cache de handles já sabe: `cbuf_reused`), reusar os ids
de view e sampler resolvidos no draw anterior, pulando `VisitImageView`/`PrepareImageView`/`GetSamplerId` das
~1245 entradas. Só é válido se **nada que afeta a resolução mudou** desde então. Pontos a investigar e cobrir
(`video_core/texture_cache/texture_cache.h`, `texture_cache_base.h`, `descriptor_table.h`):

- **Tabelas TIC/TSC**: o conteúdo das entradas pode mudar sem aviso (`DescriptorTable::Read` compara a cada
  leitura). Validação barata candidata: guardar uma cópia do trecho da tabela TIC/TSC coberto pelos índices do
  banco (min..max) e comparar com `memcmp` por draw (~40 KB; muito mais barato que 1245 `VisitImageView`). Também
  endereço/limite das tabelas (`SynchronizeDescriptors`, `texture_bindings_serial`).
- **Imagens/views criadas, apagadas ou realocadas**: `has_deleted_images`, `DeleteImage`/`UnregisterImage`,
  `slot_image_views` (ids reaproveitados após apagar!). Um contador de geração incrementado nesses pontos.
- **Escritas da CPU em memória de texturas**: invalidação marca imagens (CpuModified) e `PrepareImage` reenvia.
  Bump de geração quando uma imagem é marcada.
- **Aliases e modificação pela GPU**: `PrepareImage` sincroniza aliases (`SynchronizeAliases`) quando ticks de
  modificação diferem. `MarkModification` acontece em todo draw (render targets): **não** pode invalidar tudo; é
  preciso saber se alguma imagem do banco é também render target/alias (ex.: shadow maps amostrados pelo banco).
- **Escala (rescaling)**: `ScaleUp`/`ScaleDown`, `IsRescaling` afeta descritores e o push constant.
- **LRU/uso**: `PrepareImageView` atualiza tick de uso das imagens (coleta de lixo). Pular isso pode fazer o cache
  apagar uma imagem em uso: garantir que o tick seja atualizado (ex.: uma vez por frame para o banco inteiro).
- **Modo de verificação obrigatório**: `EDEN_HEAP_REUSE_VERIFY=1` resolve normalmente E pelo reuso, compara ids
  e loga diferenças (com o motivo provável). Usar jogando para achar pontos de invalidação esquecidos antes de
  ligar por padrão. Interruptor para desligar o reuso (`EDEN_NO_HEAP_REUSE=1`).

### Agressiva 2: reaproveitar os descritores escritos

Com a 1 valendo e as mesmas views/samplers/rescale, os ~3000 `VkDescriptorImageInfo` do bloco são idênticos aos
do draw anterior: copiar o bloco pronto (memcpy ~72 KB) em vez de montar entrada a entrada, ou, mais agressivo,
reaproveitar o `VkDescriptorSet`/região do descriptor buffer e pular a atualização no worker do Vulkan (custo de
driver na thread do worker, não medido ainda). Cuidado com o ciclo de vida da fila de descritores
(`vk_update_descriptor`, fatias por frame) e com `descriptor_allocator`.

### Descartado por escala

Heap bindless persistente no Vulkan (shader lendo o banco como no hardware): reescreve recompilador e cache de
texturas.

## Como medir

- Mesmo exe, A/B por variável de ambiente; pontos fixos: praça da cidade (vista aberta para o centro, ~15-21),
  palácio de Megaira (~20), seleção de personagem e campo (já ~30, não podem piorar).
- Perfil por thread no VS (thread de trás) antes/depois; estatísticas no log.
- Pedir prints de qualquer textura errada/piscando e testar com o interruptor desligado para isolar.

## Estado (2026-11-02, segunda sessão): agressivas 1 e 2 implementadas, ligadas por padrão

Feitas no próprio `perf/compact-texture-tables` (branch da sessão), um commit sobre `0b7ab17`.

- `texture_cache/texture_bank_reuse.h`: `DescriptorRunSnapshot` (cópia das entradas TIC/TSC que o banco lê, em
  trechos contíguos, lacunas de até 8 entradas cobertas; comparada página a página a cada reuso) e as entradas de
  banco (`ViewBankEntry`, `SamplerBankEntry`, cache LRU de 8).
- `TextureCache`: `bank_generation`, incrementado em `RegisterImage`/`UnregisterImage`, `DeleteImage`,
  `InvalidateScale` (troca de escala apaga as views), `WriteMemory`/`UnmapGPUMemory` quando uma imagem passa a
  `CpuModified`, `CreateChannel`. `TryReuseViewBank`/`StoreViewBank` (valida canal, endereço/limite da tabela,
  geração e snapshot; no reuso ainda prepara as imagens com alias e renova o LRU uma vez por frame; só guarda se
  nenhuma imagem do banco tem upload pendente e se o snapshot bate com o que a tabela resolveu).
  `TryReuseSamplerBank`/`StoreSamplerBank` (samplers nunca são apagados: só tabela e snapshot).
- `ConfigureImpl`: samplers do bloco vêm do banco; views reusadas antes do `FillImageViews`, que só resolve o
  resto; se a geração mudar durante esse resto, tudo é resolvido de novo (`retries`).
- Agressiva 2 (`PushImageDescriptors`): cada tabela guarda os descritores escritos com os carimbos (stamps) dos
  bancos e a geração; se baterem, copia o bloco (`AddEntries`).
- Interruptores: `EDEN_NO_HEAP_REUSE=1` (desliga 1 e 2), `EDEN_NO_DESCRIPTOR_REUSE=1` (só a 2),
  `EDEN_HEAP_REUSE_VERIFY=1` (resolve e escreve tudo do jeito normal, usa o resultado normal e loga diferenças:
  "Texture bank reuse check"). `EDEN_TEXTURE_TABLE_STATS=1` ganhou a linha "Texture banks" (taxa de reuso e
  motivo de cada invalidação: new/table/generation/entries/unstored/retries).
- Verificado: sintaxe (`vk_graphics_pipeline.cpp`, `vk_compute_pipeline.cpp`, instanciações Vulkan e OpenGL do
  cache de texturas); harness com ASan/UBSan: snapshot (3000 tabelas aleatórias, densas/esparsas, páginas
  faltando, mudanças dentro/fora/acima do limite) e blocos reusados contra a expansão normal e a referência
  elemento a elemento (3000 rodadas), 0 falhas.
- Falta: medir no jogo. Se a linha "Texture banks" mostrar muita invalidação por `generation`, o próximo passo é
  restringir os incrementos às imagens que estão em algum banco.

### Primeira medição (praça, reuso ligado)

- Verificação: 0 diferenças em ~38 mil conferências/s; views reusadas 99%, samplers 99,7%.
- Reuso ligado: views 95-98%, descritores 45-60%. No pior ponto da praça: **16-18 FPS, antes 13-14** (relato do
  usuário; A/B com `EDEN_NO_HEAP_REUSE=1` no mesmo exe ainda não feito).
- **Correção dos números anteriores:** antes deste commit, `compact_stats` era `static thread_local` dentro do
  template `ConfigureImpl<Spec>`, ou seja, um contador por instanciação; os "~11 mil draws/s" eram de uma só. Agora
  o contador é único: são **~45-52 mil draws/s**, dos quais só ~1,6-2,6 mil (3-5%) usam o banco. O banco não é o
  gargalo principal; o custo está espalhado pelo overhead por draw. Snapshot de ~900-1400 bytes por banco: só
  ~30-45 dos ~1000-1400 índices TIC do banco estão dentro do limite da tabela (o resto vira view nula).
- Próximo passo: perfil por thread (thread de trás) com o reuso ligado, para ver o que domina por draw.

### Perfil com o reuso ligado (praça) e o corte seguinte

- `ConfigureImpl` 48% da thread de trás. Linha mais quente: `ExpandViewLookups` com **15,25%** (escrever as ~3000
  views/samplers de cada bloco elemento a elemento). `PushImageDescriptors` 7,25%, `BindHostStageBuffers` ~5,4%,
  `UpdateGraphicsBuffers` 5,1%, `CompactTextureTable::Build` 3,3%, `IsRegionGpuModified` 3% (chamador ainda
  desconhecido), `TryReuseViewBank` 1,5%.
- Corte: os elementos dos blocos não são mais escritos (`ExpandBlockElements` só sob demanda: diagnóstico no erro do
  `PushImageDescriptors`, captura nativa, tabelas com exclusão). O caminho compacto dos descritores não lia essas
  cópias; o `CheckFeedbackLoop` recebe `compact_fill` + os ids distintos dos blocos (mesmo conjunto de ids).
  Harness: expansão adiada igual à imediata, conjunto de ids igual ao dos elementos.

### Perfil depois do corte da expansão (+1 a 3 FPS confirmados)

- `ConfigureImpl` 40,5%. `prepare_stage` 14,8% (`PushImageDescriptors` 8,5%, `BindHostStageBuffers` 6,3%),
  `UpdateGraphicsBuffers` 6,9%, `Build` 3,8%, `vcruntime` 3,5%, `IsRegionGpuModified` 3,4% (chamado por
  `InnerInvalidation`, `AccelerateInlineToMemory`, `OnCachedWrite`, `WriteMemory`: escritas inline/CPU, não por
  draw), `ntdll` 3,3%, `GetSubmappedRange` 2,7%, `GpuToCpuAddress` 2,3%. Nada acima de ~8%: o custo está espalhado.
- Corte seguinte: até 3 variantes decodificadas por descritor de array de cbuf (LRU), em vez de redecodificar e
  refazer o `Build` (e perder os descritores escritos da tabela) sempre que o array alterna. Estatística nova:
  "cbuf array variants served X% of reuses".
