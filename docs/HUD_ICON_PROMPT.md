# Prompt: ícones do HUD de toque (Ultimate Spider-Man Android)

Use este prompt em um gerador de imagens. Gere **uma imagem por arquivo**, com os nomes exatos da tabela, e depois compacte tudo em um único ZIP (`usm_hud_icons.zip`), com os PNG na raiz do ZIP e os variantes pressionados numa pasta `on/`.

## Requisitos técnicos (obrigatórios)

- Formato: **PNG com canal alfa (RGBA)**, fundo **transparente**.
- Tamanho: **256 x 256 px** exatos.
- Sem texto, sem letras, sem moldura de app. O ícone deve ser lido só pela forma.
- Estilo: botão circular de jogo mobile, glifo branco ou claro, com contorno escuro sutil, visível sobre fundo escuro e sobre o cenário colorido.
- Os glifos ocupam cerca de 70% do quadro, centralizados, com margem.
- Não copie personagens, logotipos ou marcas registradas da Marvel. Os glifos devem ser genéricos (punho, bota, teia, etc.).

## Prompt base (copie e cole, uma vez por ícone)

> Game HUD touch button icon, flat vector style, single white glyph centered on a transparent background, 256x256 px, soft dark outline for contrast, no text, no letters, no border, no background circle, no shadow on the background, generic symbol only. Glyph: {DESCRIÇÃO}.

## Ícones do botão (8 arquivos normais + 8 pressionados)

| Arquivo | Ação no jogo | Glifo (DESCRIÇÃO no prompt) |
|---|---|---|
| `hud_jump.png` | Pular (tecla Espaço) | seta para cima com barra embaixo, indicando pulo |
| `hud_web.png` | Lançar teia (tecla C) | mão aberta com linhas de teia saindo da palma |
| `hud_punch.png` | Soco (tecla Q) | punho fechado de frente |
| `hud_kick.png` | Chute (tecla E) | perna/bota chutando para a direita |
| `hud_wallcrawl.png` | Grudar na parede (tecla F) | duas mãos ou pés aderidos a uma superfície vertical |
| `hud_black.png` | Botão preto / modo especial (tecla V) | círculo com contorno em espiral |
| `hud_pause.png` | Pausa (tecla Esc) | duas barras verticais |
| `hud_back.png` | Voltar (tecla M) | seta curva apontando para a esquerda |

Pressionados (mesmo glifo, mais brilhante e com fundo preenchido em tom claro, para indicar que o botão está apertado):

| Arquivo | Base |
|---|---|
| `on/hud_jump_on.png` | `hud_jump.png` pressionado |
| `on/hud_web_on.png` | `hud_web.png` pressionado |
| `on/hud_punch_on.png` | `hud_punch.png` pressionado |
| `on/hud_kick_on.png` | `hud_kick.png` pressionado |
| `on/hud_wallcrawl_on.png` | `hud_wallcrawl.png` pressionado |
| `on/hud_black_on.png` | `hud_black.png` pressionado |
| `on/hud_pause_on.png` | `hud_pause.png` pressionado |
| `on/hud_back_on.png` | `hud_back.png` pressionado |

Para os pressionados, use o mesmo glifo do normal como imagem de referência e peça: "same glyph, brighter, filled light circle behind it, pressed state".

## Fora do escopo (não gerar)

- Analógicos: `hud_stick.png`, `hud_stick_knob.png`. Não precisam de imagem; o código desenha discos.
- Cursor e câmera: `hud_cursor.png`, `hud_campad.png`. Não precisam de imagem; o código desenha discos.
- `hud_skip.png`: o código o carrega, mas nenhum botão o usa hoje.

## Montagem do ZIP

Estrutura final (nomes exatos, sem pasta extra no topo):

```
usm_hud_icons.zip
├── hud_jump.png
├── hud_web.png
├── hud_punch.png
├── hud_kick.png
├── hud_wallcrawl.png
├── hud_black.png
├── hud_pause.png
├── hud_back.png
└── on/
    ├── hud_jump_on.png
    ├── hud_web_on.png
    ├── hud_punch_on.png
    ├── hud_kick_on.png
    ├── hud_wallcrawl_on.png
    ├── hud_black_on.png
    ├── hud_pause_on.png
    └── hud_back_on.png
```

Depois de descompactar em `<jogo>/port/icons/`, o log deve mostrar `touch: 16 HUD icons loaded` (8 normais + 8 pressionados). Se mostrar menos, algum nome está errado.

## Verificação antes de enviar

1. Abrir cada PNG e confirmar 256x256 e fundo transparente.
2. Conferir que os nomes batem exatamente com a tabela (minúsculas, underline, sem espaço).
3. Confirmar que não há arquivo extra além dos 16 listados.
