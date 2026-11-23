# Bibliothèques graphiques des jeux : quoi remplacer pour supprimer les listes de commandes PICA200

Analyse du 2026-11-23, faite sur le code décompressé des ROM (rien n'a été lancé).
Le but est le point 3 du principe « aucune émulation » de `PLAN.md` : les jeux ne
doivent plus fabriquer de listes de commandes PICA200 que picanative relit.

Chaque affirmation porte une marque :

- **[vu]** : vérifié dans le code ou les données des ROM (adresse, symbole, comptage) ;
- **[supposé]** : déduit de ce qu'on sait des formats et du SDK, pas vérifié ici.

Les adresses sont celles du code chargé à 0x00100000 (ROM européennes, révisions
de `C:\jeu 3ds\extrait` et de `Downloads`). Les scripts sont dans le dossier de
notes de la session (`bibliotheques-outils/`, voir la fin).

## En bref

1. **Tous les jeux passent par le même bas de pile, `nn::gx` du SDK** [vu] :
   `nngx*` (listes de commandes, écrans, transferts), puis six enveloppes de
   commandes GX, puis la file de commandes du GSP en mémoire partagée. C'est là que
   ctrnative coupe aujourd'hui (`src/host/gsp.cpp`).
2. **Personne n'utilise vraiment l'API OpenGL de `nn::gx`** [vu, à nuancer] : les
   constantes GL (`GL_ARRAY_BUFFER`, `GL_FRAMEBUFFER`...) sont chargées 0 à 5 fois
   par jeu. Les moteurs écrivent les registres PICA eux-mêmes dans la liste
   courante. Couper « sous » le moteur (au niveau GL) ne servirait donc à rien :
   il faut couper **dans chaque moteur**.
3. **Les fichiers de modèles contiennent déjà des commandes PICA toutes faites**
   [vu pour CGFX] : remplacer la bibliothèque ne suffit pas si on la remplace par
   du code qui relit ces blocs à chaque image. La version native doit lire les
   champs structurés des ressources, ou **traduire ces blocs une fois, au
   chargement** (comme les shaders), jamais pendant la partie.
4. Cinq familles de bibliothèques couvrent les dix jeux :

| Bibliothèque | Jeux | Couvre |
|---|---|---|
| NintendoWare `nw::gfx` (3D, CGFX) | SM3DL (ancienne version), NSMB2, MK7, Pokémon X/Y, Rubis Oméga/Saphir Alpha | 5 jeux |
| NintendoWare `nw::lyt` + `nw::font` (2D) | NSMB2, MK7, SM3DL, X/Y, ORAS, Soleil, Ultra-Lune (2 versions au moins) | 7 jeux, menus et textes |
| NintendoWare `nw::h3d` (3D, BCH) | X/Y, ORAS (sous `gfl::grp::g3d`), Smash | 3 jeux |
| Game Freak `gfl2::gfx` (GFGL, façon Direct3D) | Soleil, Ultra-Lune | 2 jeux |
| Grezzo `gzw` | Zelda OoT 3D, Zelda MM3D (deux versions éloignées) | 2 jeux |
| Bandai Namco/Sora `lib` (`Renderer`, `Gsb*`, `Lumen`) | Smash | 1 jeu |

5. **Ordre conseillé** (détail plus bas) : le socle `nn::gx`, puis `nw::gfx`
   (version NSMB2/MK7), `nw::lyt`/`nw::font`, Grezzo, puis, une fois les modules
   CRO chargés, `gfl2::gfx`, `h3d`, et Smash.

## 1. Ce que chaque jeu embarque

### Identité et âge du SDK

Le numéro de version du SDK n'est écrit nulle part dans le code (aucune chaîne
`CTR_SDK`) [vu]. On le date par la version minimale du noyau demandée dans
l'exheader [vu] et par la ressemblance du code `nngx` avec celui de Pokémon Soleil
(dont les fonctions `nngx*` sont nommées dans son CRS) [vu] :

| Jeu | Nom interne (exheader) | Noyau minimal | Code `nngx` identique à Soleil (sur 27 fonctions) |
|---|---|---|---|
| SM3DL | CtrApp | 2.30 | 1 seule entière, 5 partielles : SDK de 2011 |
| Zelda OoT 3D | CtrApp | (absent) | 1 partielle sur 23 mots : SDK de 2011 |
| NSMB2 | BIGRED | 2.32 | 9 entières |
| Mario Kart 7 | MarioKar | 2.32 | 9 entières |
| Pokémon X/Y | kujira-2 | 2.35 | 14 entières |
| Rubis Oméga / Saphir Alpha | sango-1 / sango-2 | 2.39 | (comme X/Y, 80 % du code commun avec X/Y) |
| Zelda MM3D | Joker | 2.39 | 12 entières |
| Smash 3DS | ctrapp | 2.50 | 8 entières |
| Pokémon Soleil / Ultra-Lune | niji_loc / momiji | 2.50 | référence / 13 entières |

Trois générations de `nn::gx` donc [vu] : 2011 (SM3DL, OoT), 2012 (NSMB2, MK7),
2013-2016 (le reste, quasi identique). La correspondance noyau → version du SDK
précise est [supposé].

### Part de code identique entre jeux

Pourcentage des fonctions appelées au moins deux fois (10 premiers mots, branches
et littéraux masqués) qu'on retrouve à l'identique dans l'autre jeu [vu] :

| | SM3DL | NSMB2 | MK7 | OoT | MM | X/Y | ORAS | Soleil | UL | Smash |
|---|---|---|---|---|---|---|---|---|---|---|
| SM3DL | 100 | 6,8 | 8,0 | 9,2 | 1,7 | 4,0 | 3,9 | 3,0 | 3,0 | 2,9 |
| NSMB2 | 7,2 | 100 | 22,8 | 1,7 | 9,2 | 18,1 | 16,8 | 11,7 | 11,8 | 11,0 |
| MK7 | 7,8 | 19,0 | 100 | 3,6 | 9,1 | 19,0 | 18,3 | 14,3 | 14,3 | 13,2 |
| OoT | 13,6 | 2,1 | 6,4 | 100 | 5,7 | 5,1 | 4,9 | 4,5 | 4,6 | 4,0 |
| MM | 1,9 | 9,4 | 11,2 | 4,3 | 100 | 14,0 | 15,4 | 14,4 | 14,4 | 9,6 |
| X/Y | 3,3 | 15,0 | 19,5 | 2,7 | 10,6 | 100 | 83,5 | 39,0 | 38,9 | 27,2 |
| ORAS | 3,1 | 13,2 | 17,9 | 2,5 | 11,1 | 80,8 | 100 | 39,6 | 39,5 | 26,7 |
| Soleil | 2,2 | 8,1 | 13,0 | 2,1 | 9,5 | 36,0 | 37,9 | 100 | 90,5 | 25,6 |
| UL | 2,2 | 7,9 | 12,7 | 2,1 | 9,2 | 35,3 | 37,2 | 89,0 | 100 | 25,4 |
| Smash | 1,1 | 4,2 | 6,4 | 1,0 | 3,7 | 12,7 | 13,2 | 13,7 | 13,8 | 100 |

À retenir : X/Y ≈ ORAS, Soleil ≈ Ultra-Lune ; OoT et MM n'ont presque rien en
commun (même moteur Grezzo, mais réécrit et recompilé) ; SM3DL et OoT ne
partagent que leur SDK de 2011.

### Jeu par jeu

**Super Mario 3D Land** — moteur EAD Tokyo (`sead`, `al`, cf. sm3dl-decomp) sur
NintendoWare 4C.
- `nw::gfx` ancienne version [vu] : seules 23 des 345 méthodes virtuelles
  `nw::gfx` de Pokémon X/Y se retrouvent à l'identique (contre 113 à 131 dans
  NSMB2/MK7). Chaînes `NW4C_ROOT`, `NW4C_CreativeStudioConsoleError.exe`,
  `nw::lyt::Layout::CalculateMtx` [vu].
- Modèles CGFX dans les `.szs` : sur 140 `.szs` échantillonnés, 88 contiennent
  du CGFX, avec 2 004 en-têtes de blocs TEV PICA (`0x804F00C0`...) tout faits [vu].
- Pas de RTTI : les classes ne sont pas nommées dans le binaire.

**New Super Mario Bros. 2** (`BIGRED`) — moteur EAD (chemins `d:\home\bigred\...`).
- `nw::gfx` [vu] : 113/345 méthodes de X/Y identiques ; 392 fichiers `.bcres`
  (CGFX) ; 2 670 blocs TEV tout faits dans 30 `.bcres` (témoin : 2 dans 34 Mo de
  `.bcstm`) [vu].
- `nw::lyt` (24/137), `nw::font` (10/70), `nw::snd` [vu].
- Code partiellement en Thumb (boucle de constructeurs Thumb, cf. `ctrn.py`) : les
  comptes d'appels en Thumb ne sont pas inclus ci-dessous.

**Mario Kart 7** — moteur Retro/EAD.
- `nw::gfx` [vu] : 131/345 ; 309 `.bcmdl`, `.bctex`, `.bcmata` (morceaux de CGFX),
  264 blocs TEV dans 30 `.bcmdl`. `nw::anim`, `nw::lyt`, `nw::font` [vu].
- C'est la même génération que NSMB2 (22 % du code identique) [vu].

**Zelda Ocarina of Time 3D** — moteur Grezzo `gzw` (`d:\home\queen\...`).
- Fichiers sources nommés dans les chaînes [vu] : `libraries\gzw\graph\CmbRenderer.cpp`,
  `BoardRenderer.cpp`, `ShaderProgram.cpp`, `AnimatedMaterial.cpp`, `Skeleton.cpp`,
  `gzw\res\CmbRes.cpp`, **`gzw\sys\PicaCommand.cpp`** (le fabricant de commandes),
  `rom:/CmbVShader.shbin`.
- Aucune trace de NintendoWare graphique (0 méthode `nw::gfx`/`lyt`) [vu].
- Formats maison `.zar`/`.cmb`/`.zsi`/`.ctxb` : **aucun bloc TEV tout fait** dans
  40 `.zar` [vu] — les commandes sont fabriquées par le code.

**Zelda Majora's Mask 3D** (`Joker`, Jenkins `joker-1_1_0`) — Grezzo, version
2015 : mêmes formats (`.zsi`, `.ctxb`, `.gar.lzs`), `rom:/shaders/GzwShader.shbin`,
`LayoutShader.shbin`, `Layout\Joker\LayoutDrawManager.cpp` [vu]. 4 % seulement du
code commun avec OoT [vu] : à traiter comme une bibliothèque distincte. Aucun bloc
TEV tout fait dans 40 `.gar.lzs` [vu].

**Pokémon X/Y** (`kujira`) et **Rubis Oméga/Saphir Alpha** (`sango`) — Game Freak
`gflib` (`c:\home\gflib_cpp_final\...`), modules CRO.
- `gfl::grp::g3d` au-dessus de **deux** moteurs 3D NintendoWare [vu] :
  `nw::gfx` (classes `nw::gfx::MeshRenderer`, `RenderContext`, `ShaderProgram`,
  `MaterialActivator`, `SceneUpdater`... en RTTI) et `h3d`
  (`gfl::grp::g3d::H3dModel`, `H3dResShader`, `H3dResMaterialAnim`,
  `rom:/g3d_common/*.bcmdl.h3d`).
- **Lecture des commandes PICA des ressources h3d par le jeu** [vu, par les noms] :
  `H3dCommandListenerReadShader`, `H3dCommandListenerReadMaterial`,
  `H3dCommandListenerReadTexture0`, `DrawUtil::Debug::RegisterStatus::ReadCommand`.
- 2D : `nw::lyt`, `nw::font`, `nw::anim`. ORAS = X/Y à 80 % (224/345 méthodes
  `nw::gfx` de X/Y identiques dans ORAS) [vu].
- Le CRS n'exporte que la bibliothèque C++ standard (461 noms) : les CRO importent
  le reste par index [vu].

**Pokémon Soleil / Ultra-Lune** (`niji`, `momiji`) — Game Freak `gfl2`, 117/134 CRO.
- `gfl2::gfx` : une **couche d'abstraction façon Direct3D** avec un dos « CTR »
  [vu, RTTI et exports] : `BlendStateObject`, `RasterizerStateObject`,
  `DepthStencilStateObject`, `SamplerStateObject`, `VertexBuffer`, `IndexBuffer`,
  `ConstantBuffer`, `DisplayList`, et leurs versions `ctr::CTRxxx` ; `ctr::CTRGL`
  (`BeginScene_`, `EndScene_`, `SetRenderTarget_`, `SetViewport_`, `SetTexture_`,
  `ClearRenderTargetDepthStencil_`, `FlushAllDirtyCommand_(nn::gr::CTR::CommandBufferJumpHelper*)`...),
  `ctr::CTRCommandList`, `CTRCommandBuffer<16/32/544>`.
- Au-dessus : `gfl2::renderingengine` (scène, `DrawManager`, `RenderingPipeLine`,
  ~2 000 noms) et `poke_3d` [vu].
- En dessous : la bibliothèque **`nn::gr`** du SDK (aides pour composer les
  commandes ; `nn::gr::CTR::CopyMtx34WithHeader` 0x109328 exporté) [vu].
- Les 27 fonctions `nngx*` sont exportées par le CRS avec leur adresse (table plus
  bas) [vu]. Aucune constante GL dans le code [vu].
- 2D : `nw::lyt`, `nw::font`, `nw::eft` d'une version plus récente que celle de X/Y
  (5/137 méthodes `lyt` de X/Y identiques) [vu].

**Super Smash Bros. for 3DS** — moteur `lib` (Sora/Bandai Namco).
- `lib::Renderer` (53 fonctions exportées : `draw_texture_polygon`,
  `set_render_state`, `set_render_target`, `enable_planar_shadow`...),
  `lib::GsbDrawModel` (37), `GsbDrawSceneManager`, `GsbResourceManager`,
  `EffectSystem`, `LumenPlayer`/`LumenFont` (interface) [vu, exports du CRS].
- `nw::h3d` [vu] : `nw::h3d::SkeletonState::UpdateWorldMtxsImpl`,
  `nw::h3d::res::AnimFloatVectorElementData` exportés.
- Pas de `nw::gfx`, pas de `nw::lyt` (0 méthode de X/Y) [vu].
- Les ressources sont dans un paquet unique (`dt`/`ls`) : pas vérifiées ici.

## 2. Où passent les listes de commandes

### Le chemin d'envoi, identique partout [vu]

```
moteur (nw::gfx, gfl2, gzw, lib...)  écrit les registres PICA dans la liste courante
  └─ nngxRunCmdlist / nngxSplitDrawCmdlist / nngxFlush3DCommand / nngxSwapBuffers ...
       └─ répartiteur des requêtes nngx (type 0 à 4 = commande GX)
            └─ 6 enveloppes GX : RequestDma, ProcessCommandList(+FlushCacheRegions),
               MemoryFill, DisplayTransfer, TextureCopy
                 └─ ajout dans la file GX (mémoire partagée du GSP)
                      └─ gsp::Gpu TriggerCmdReqQueue (IPC 0x000C0000)
                           └─ ctrnative : Gsp::process → pn_command_list (picanative)
```

Adresses trouvées (la file GX par l'IPC `TriggerCmdReqQueue` ; le reste par
correspondance avec le code nommé de Soleil ; entre parenthèses, le nombre d'appels
`BL` directs en ARM) :

| Jeu | `nngxRunCmdlist` | `nngxSwapBuffers` | `nngxWaitCmdlistDone` | répartiteur nngx | file GX | enveloppes GX (DMA…DisplayTransfer) |
|---|---|---|---|---|---|---|
| SM3DL | 0x284894 (17) | ? | ? | ? | 0x28946c | 0x10a8b8 … 0x10ad58 |
| NSMB2 | 0x541108 (23) | 0x541390 (4) | 0x541e20 (22) | 0x11406c | 0x11aa7c | 0x1181fc … 0x118634 |
| MK7 | 0x58b778 (31) | 0x58b994 (6) | 0x58c54c (32) | 0x111540 | 0x113c9c | 0x1132dc … 0x113714 |
| OoT | 0x311954 (2) | ? | ? | ? | 0x2c188c | 0x4939ac … 0x493d60 |
| MM | 0x15bab8 (1) | 0x14887c (1) | 0x148a14 (1) | 0x148fac | 0x153f18 | 0x13ecec … 0x13f18c |
| X/Y | 0x130db4 (1) | 0x130e5c (1) | 0x143454 (10) | 0x15d5f8 | 0x172eec | 0x163dc0 … 0x164258 |
| ORAS | 0x131084 (1) | 0x13112c (1) | 0x142624 (10) | 0x15d8f8 | 0x1740c4 | 0x164894 … 0x164d2c |
| Soleil | 0x523fbc (0 ¹) | 0x524494 | 0x524d98 (4) | 0x53bb90 | 0x278298 | 0x2781a4 … 0x278e28 |
| Ultra-Lune | 0x5479e4 (0 ¹) | 0x547ebc | 0x5487c0 (4) | 0x55f758 | 0x27990c | 0x279818 … 0x27a49c |
| Smash | 0x181a04 (28) | ? | 0x16f8c8 (26) | 0x181e40 | 0x186df0 | 0x175f08 … 0x1762e4 |

¹ appelée autrement que par `BL` (pointeur ou saut), pas cherché plus loin.
« ? » : le code de 2011 diffère trop de celui de Soleil pour la correspondance
automatique ; à trouver à la main en remontant depuis la file GX.

Lecture des appels : chez NSMB2, MK7, SM3DL et Smash, `nngxRunCmdlist` est appelée
de 17 à 31 endroits (NintendoWare et le moteur envoient eux-mêmes) ; chez Grezzo
et Game Freak, d'un seul endroit (le moteur centralise l'envoi) [vu].

Autres fonctions `nngx` de Soleil (nommées par leur place dans la bibliothèque,
**[supposé]**) : 0x521e94-0x5220f4 = les six `nngxAdd*Command`,
0x5230e8 (12 appels) = `nngxGetCmdlistParameteri`, 0x523ef4 (10 appels) =
`nngxMoveCommandbufferPointer`, 0x52417c = `nngxSplitDrawCmdlist`. Ce sont les
fonctions par lesquelles un moteur obtient l'adresse courante de la liste et y
écrit directement.

### Qui écrit les commandes

Aucun jeu n'écrit par l'API GL de `nn::gx` de façon notable [vu] : les constantes
GL n'apparaissent que 0 à 5 fois (Soleil/UL : 0 ; Smash : 2 ; OoT et MM, un peu
plus : `GL_ARRAY_BUFFER` ×3/×2, `GL_FRAMEBUFFER` ×2/×3). La table des noms
d'uniformes `dmp_*` de la couche GL est liée dans tous les jeux (297 à 393 noms),
mais sa présence ne prouve pas son usage. **À vérifier** : si OoT/MM passent une
partie de leur état par `glUniform*` (le nom `ShaderProgram.cpp` de gzw le
suggère).

Les écritures sont donc dans les moteurs, en trois formes :

1. **Le moteur compose les commandes avec du code** : Grezzo
   (`gzw\sys\PicaCommand.cpp`) [vu, par le nom], `gfl2::gfx::ctr` avec `nn::gr`
   [vu], `lib::Renderer` de Smash [supposé].
2. **Le moteur copie ou « appelle » des blocs tout faits venus des fichiers** :
   CGFX de `nw::gfx` (blocs TEV comptés dans SM3DL, NSMB2, MK7) [vu] ; BCH de
   `h3d` (X/Y, ORAS, Smash) [vu chez X/Y par les noms `H3dCommandListener*`,
   supposé chez Smash] ; modèles de `gfl2` [supposé]. `nn::gr::CTR::CommandBufferJumpHelper`
   dans `gfl2` [vu] montre que Soleil fait sauter le GPU dans des tampons de
   commandes préparés à l'avance (registres de saut 0x238-0x23D).
3. **Le code de la plateforme du SDK** : les transferts et remplissages
   (`MemoryFill`, `DisplayTransfer`, `TextureCopy`) passent tous par les
   enveloppes GX ci-dessus [vu].

Un comptage brut des fonctions qui chargent des valeurs ressemblant à des en-têtes
de commandes PICA donne 90 (OoT) à 470 (Smash) fonctions par jeu, mais il ramasse
aussi des en-têtes IPC : **ce chiffre n'est qu'un ordre de grandeur**.

## 3. À quel niveau remplacer, bibliothèque par bibliothèque

Principe commun : on remplace la fonction de la bibliothèque (par un override de
3dsrecomp, comme les services) par une fonction native qui fait le travail avec
l'API GPU du PC (le Dawn/WebGPU de picanative) au lieu d'écrire des commandes. Le
niveau idéal est **le plus haut niveau stable de la bibliothèque** : celui où l'on
parle de maillages, matériaux, textures et cibles de rendu, pas de registres.

### 3.0 Le socle `nn::gx` (les 10 jeux)

- **Quoi** : les `nngx*` (listes, tampons d'affichage, `SwapBuffers`,
  `TransferRenderImage`, `Add*Command`), le répartiteur et les 6 enveloppes GX.
- **Ce que ça apporte** : transferts, remplissages et présentation à l'écran en
  natif, sans passer par la file du GSP ; un point d'accroche unique pour savoir
  quelle bibliothèque a écrit quoi. **Ça ne supprime pas les listes** : tant que
  le moteur au-dessus n'est pas remplacé, `RunCmdlist` doit toujours confier la
  liste à picanative.
- **Taille** : ~60 fonctions, une API publique et documentée par son usage ;
  trois générations (2011, 2012, 2013+) à repérer. **Petit** (une à deux semaines),
  dont l'essentiel est le repérage automatique des adresses par jeu.
- Préalable utile : **un traceur d'attribution** (adresse de retour de chaque
  écriture dans la liste → fonction du jeu) pour mesurer, image par image, la part
  de commandes encore fabriquées par chaque bibliothèque. Sans lui, on ne peut
  pas dire qu'une bibliothèque est « faite ».

### 3.1 NintendoWare `nw::gfx` (SM3DL, NSMB2, MK7, X/Y, ORAS)

- **Niveau** : `nw::gfx::RenderContext` (activation caméra, matériau, shader,
  lumières, cibles) et `MeshRenderer`/`MaterialActivator` (dessin d'un maillage)
  [classes vues en RTTI chez X/Y]. Le graphe de scène, les animations et le
  calcul des matrices restent du code du jeu traduit.
- **Données** : les CGFX contiennent les paramètres structurés **et** des
  « caches de commandes » [blocs vus ; doublon structuré/cache supposé d'après le
  format]. La version native lit les champs structurés et ignore les caches.
- **Versions** : trois au moins — SM3DL (2011), NSMB2/MK7 (2012, 113-131
  méthodes identiques à X/Y), X/Y/ORAS. Comme ces jeux sont sans symboles (sauf
  RTTI chez Pokémon), il faut repérer les fonctions par correspondance avec X/Y.
- **Taille** : **grosse**. Plusieurs centaines de fonctions touchent au rendu ;
  compter 2 à 3 mois pour la première version (matériaux complets : TEV, lumières
  de fragment, tables LUT, brouillard, ombres), puis quelques semaines par version
  supplémentaire.

### 3.2 NintendoWare `nw::lyt` + `nw::font` (7 jeux, 2D)

- **Niveau** : `nw::lyt::DrawInfo` / dessin des `Pane` (`Picture`, `TextBox`,
  `Window`) et `nw::font::RectDrawer`/`TextWriter` : des quadrilatères texturés
  avec un matériau 2D [classes vues en RTTI chez X/Y et Soleil].
- **Versions** : celle de NSMB2/MK7, celle de X/Y/ORAS, celle de Soleil/UL ;
  SM3DL ancienne [vu par les correspondances]. MM a son `LayoutDrawManager` à lui
  (probablement Grezzo, pas NintendoWare : 0 méthode commune) [vu/supposé].
- **Taille** : **moyenne**, 3 à 5 semaines pour une version (peu de fonctions de
  dessin, matériaux 2D limités), puis une à deux semaines par version.
- Intérêt : les menus et textes de 7 jeux ; utile même quand la 3D passe encore
  par picanative.

### 3.3 Grezzo `gzw` (OoT, MM)

- **Niveau** : `CmbRenderer` / `BoardRenderer` / `ShaderProgram` /
  `AnimatedMaterial` (OoT) [noms de fichiers vus], c'est-à-dire « dessiner un CMB
  avec son matériau », et `PicaCommand` en dessous.
- **Données** : aucun bloc de commandes tout fait dans les ressources [vu] — c'est
  le cas le plus simple pour « aucune liste » : tout part de champs structurés.
- **Versions** : OoT et MM sont deux bibliothèques (4 % de code commun) [vu].
  MM n'a pas les chemins `libraries\gzw` dans ses chaînes, il faudra retrouver ses
  fonctions autrement (par `GzwShader.shbin`, les formats, ou en partant de
  `nngxRunCmdlist`, appelée d'un seul endroit).
- **Taille** : **moyenne** par jeu, 1 à 2 mois pour OoT, autant pour MM
  (repérage plus long : pas de symboles, pas de RTTI).

### 3.4 Game Freak `gfl2::gfx` (Soleil, Ultra-Lune)

- **Niveau** : l'interface `gfl2::gfx` elle-même. C'est la meilleure coupe de tous
  les jeux [vu] : une API façon Direct3D (objets d'état, tampons de sommets et
  d'indices, tampons de constantes, textures, cibles, `BeginScene`/`EndScene`)
  avec un dos `ctr::CTR*` qu'on remplace par un dos natif, sans toucher au moteur
  au-dessus.
- **Points durs** [supposé] : `DisplayList`/`CTRCommandBuffer` et
  `CommandBufferJumpHelper` (tampons préparés et rejoués par saut) ; les shaders
  de pixels `CTRPixelShader` sont sans doute des blocs de commandes de l'étage de
  fragments ; les modèles `GfModel` contiennent probablement des blocs tout faits
  → à traduire au chargement.
- **Versions** : une seule (Soleil ≈ UL à 90 %) [vu].
- **Taille** : **moyenne**, 1 à 2 mois, deux jeux d'un coup ; facilitée par les
  noms exportés du CRS (4 543 et 4 916 noms).
- **Bloquant** : les Pokémon s'arrêtent aujourd'hui à l'image 1 (modules CRO,
  `ldr:ro`). Rien à mesurer tant que ce n'est pas réglé.

### 3.5 NintendoWare `h3d` (X/Y, ORAS, Smash)

- **Niveau** : le dessin d'un `H3dModel` (maillage + matériau + shader) ; chez
  Pokémon par `gfl::grp::g3d::H3d*` [vu], chez Smash par `lib::GsbDrawModel` [vu]
  au-dessus de `nw::h3d` [vu].
- **Données** : dans BCH, matériaux et shaders sont stockés essentiellement sous
  forme de commandes PICA [vu chez X/Y par `H3dCommandListenerRead*` ; le détail du
  format est supposé]. La version native doit donc **décoder ces blocs une fois au
  chargement** en état natif (un décodeur hors ligne, pas un interpréteur par
  image) — le `CommandProcessor` de picanative peut servir de base.
- **Taille** : **grosse** (décodeur d'état + moteur de dessin), 2 à 3 mois ; puis
  l'adaptation à Smash, qui a sa propre couche `Gsb`.
- **Bloquant** : CRO (Pokémon) et chargement de Smash (meurt à l'image 1).

### 3.6 Smash `lib` (1 jeu)

- **Niveau** : `lib::Renderer` (53 fonctions exportées, API 2D/3D de haut niveau),
  `GsbDrawSceneManager`, `EffectSystem`, `Lumen*` pour l'interface [vu].
- **Taille** : **grosse** et pour un seul jeu ; à faire en dernier.

### Ce qui reste hors des bibliothèques

- **Les shaders de sommets** (`.shbin`, présents dans tous les jeux [vu]) : c'est
  le point 2 du principe (traduction à l'avance), pas une bibliothèque.
- **Le code du jeu qui écrit des registres lui-même** (effets maison, écrans de
  transition, `nw::eft`, `gfl2` hors `gfx`) : seul le traceur d'attribution (3.0)
  dira combien il en reste, jeu par jeu.

## 4. Ordre conseillé

Critère : le plus de jeux couverts au plus tôt, en commençant par ceux qui tournent
déjà dans ctrnative (SM3DL, NSMB2, OoT, MM ; MK7 pas encore essayé ; Pokémon et
Smash bloqués par les CRO).

| Étape | Quoi | Jeux qui en profitent | Taille | Ce qui est supprimé |
|---|---|---|---|---|
| 1 | Socle `nn::gx` + traceur d'attribution | 10 | petite | file GSP, transferts ; mesure de ce qui reste |
| 2 | `nw::gfx` version 2012 | NSMB2, MK7 | grosse | la 3D de deux jeux |
| 3 | `nw::lyt` + `nw::font` version 2012 | NSMB2, MK7 | moyenne | leur 2D → **deux jeux sans liste** (sauf code maison) |
| 4 | `nw::gfx` + `lyt` version 2011 | SM3DL | moyenne (adaptation) | le jeu de référence |
| 5 | Grezzo OoT, puis MM | OoT, MM | moyenne ×2 | deux jeux, sans blocs dans les données |
| 6 | CRO dans ctrnative (hors ce sujet), puis `gfl2::gfx` | Soleil, UL | moyenne | deux jeux par une seule API propre |
| 7 | `nw::gfx`/`lyt` version X/Y + `h3d` | X/Y, ORAS | grosse | quatre jeux |
| 8 | Smash (`lib` + `h3d`) | Smash | grosse | un jeu |

Variante : si les CRO arrivent tôt, l'étape 6 (`gfl2`) passe avant les Zelda —
c'est le meilleur rapport travail/jeux couverts.

SM3DL n'est pas en tête, bien qu'il soit le jeu de référence : sa version de
NintendoWare est la plus ancienne et la moins partagée (7 % de code commun avec
NSMB2/MK7). On le fait juste après la version 2012, en adaptation.

## Limites de cette analyse

- Correspondances de code par les premiers mots des fonctions (branches et
  littéraux masqués) : sûres quand elles trouvent, muettes entre versions éloignées
  (d'où les « ? » de 2011).
- Comptes d'appels : seulement les `BL` directs ; ni les pointeurs de fonctions, ni
  les sauts de fin de fonction, ni le Thumb (NSMB2 en a).
- Ressources vérifiées : CGFX (SM3DL, NSMB2, MK7) et Grezzo (OoT, MM). Les GARC des
  Pokémon et le paquet de Smash ne l'ont pas été.
- Aucun jeu n'a été lancé : rien ici ne dit quelle part des commandes d'une image
  vient de quelle bibliothèque. C'est le rôle du traceur de l'étape 1.
- Les tailles de travail sont des estimations d'ordre de grandeur.

## Refaire l'analyse

Scripts Python (capstone) dans
`%LOCALAPPDATA%\Claude-Data\...\side-session-notes\<session>\bibliotheques-outils\` :
`extract.py <dossier>` (code + CRO/CRS de chaque ROM, avec `ctrn.read_code`),
`cro.py` (exports/imports CRO), `sigmatch.py` (fonctions nommées de Soleil
retrouvées ailleurs), `vtables.py` (méthodes virtuelles nommées par la RTTI d'un
jeu, cherchées dans les autres), `simall.py` (code commun entre jeux),
`gxwrappers.py` (file GX et enveloppes), `addrmatch.py`, `rescmd.py` (blocs PICA
dans les ressources). `arm.py` lit le code dans `../out/<jeu>/code.bin`.
