# Variáveis de ambiente que restaram

Desde 2026-11-03 o fork não segue mais upstream, e o código de diagnóstico da fase de bring-up foi removido
(capturas `EDEN_NATIVE_*`, traces, profilers, experimentos A/B). Ficaram só estas:

| Variável | Efeito |
| --- | --- |
| `EDEN_NO_HEAP_REUSE=1` | Desliga o reaproveitamento de bancos de texturas e dos descritores (para isolar textura errada). |
| `EDEN_TEXTURE_TABLE_STATS=1` | Loga a cada 4 s as estatísticas das tabelas de texturas e dos bancos. |
| `EDEN_BACK_THREAD_STATS=1/2` | Loga a cada 4 s onde a thread que prepara os draws gasta o tempo (trechos do `PrepareDraw`, descritores, esperas no scheduler, worker do Vulkan, locks dos caches, frente do split). `2` serializa os carimbos (`lfence`), para conferir um número decisivo. Custa ~1-2%: medir draws/s com ele desligado. Ver `docs/back-thread-split-plan.md`, fase 0. |
| `EDEN_CPU_WRITE_PAGES=N` | **Rejeitado** (deixa o jogo travado, "parece 10 FPS"; não usar). Experimento (padrão 1): as últimas N páginas (1-64) por núcleo em que uma escrita da CPU do jogo foi segurada são seguradas de novo sem consultar o buffer cache (hoje só a última), até as escritas seguradas desse núcleo serem aplicadas (início de cada command list ou fence); depois disso, cada página é consultada de novo. Menos idas ao lock dos caches; as escritas seguradas são aplicadas antes da próxima submissão, como já acontece. |
| `EDEN_INLINE_DEFER=1/2` | Experimento (desligado por padrão; vem do `EDEN_NX2_INLINE_DEFER` do PokeDinner): um upload inline que não precisa ir direto para o buffer (região sem dados escritos pela GPU) deixa para o próximo `FlushCaching` a invalidação dos caches de buffer e de shader (`1`, seguro) ou também a do cache de texturas (`2`, como no PokeDinner: uma leitura da CPU que baixa uma imagem escrita pela GPU antes do próximo `FlushCaching` desfaria a escrita). |
| `EDEN_FLUSH_DRAWS=N` | Período de envio do trabalho à GPU real, em draws (32 a 1048576, múltiplo de 32). **Padrão: 128 nos títulos do Switch 2** (desktop; de 226162edf do PokeDinner), ~4128 nos outros. Encurta o `Finish` por frame do FE (760 → 255 µs). `4096` volta ao período antigo. |
| `EDEN_HELD_WRITE_MERGE=0` | Desliga a junção das escritas seguradas do jogo antes de aplicá-las aos caches (ligada por padrão: regiões contíguas ou sobrepostas na mesma página de 4 KB viram uma; na praça, ~400 regiões por lote viram ~38). |
| `EDEN_GPU_SPLIT=0/1/2` | GPU emulada em uma thread (0), métodos na thread de trás (1) ou também macros na da frente (2, padrão). |
| `EDEN_MME_JIT=0` | Macros nativas no interpretador em vez do código compilado. |
| `EDEN_NATIVE_FRONTEND=lower/ampere/auto/compare` | Frontend de shaders nativos (padrão `lower`). |
| `EDEN_WINDOW_TITLE` | Título da janela no eden-cli (usado internamente pelo próprio frontend). |

Os testes em `src/tests/video_core/native_shader.cpp` ainda leem `EDEN_NATIVE_SHADER_FIXTURES` e afins (opcionais,
só para rodar os testes com fixtures).
