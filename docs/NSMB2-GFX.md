# NSMB2 : la 3D `nw::gfx` champ par champ (2026-11-23)

Même méthode que [NSMB2-LYT.md](NSMB2-LYT.md) : analyse statique du code de NSMB2 (base 0x100000), sans exécution.

Légende :
- **V** : lu dans le code ;
- **S+** : interprétation bien étayée (nom ou rôle) ;
- **S** : supposé.

« Liste » désigne le pointeur d'écriture courant de `nngx` (global 0x5f1dc0). Les noms viennent de la structure de `nw::gfx`. NSMB2 n'a pas de RTTI : les noms sont donc tous S+ au mieux.

Le contenu des blocs préparés n'est pas décodé mot à mot, et plusieurs fonctions annexes restent à lire (voir « Limites »).

## 0. Les petites copies : aides d'écriture d'uniformes (V)

Ce sont des fonctions feuilles qui lisent N vec4 (ordre mémoire `m0..m3`) et les écrivent **retournés** (`m3, m2, m1, m0`), l'ordre des uniformes PICA. Elles servent seulement de « tuyau » : leur contenu dépend de l'appelant.

| Adresse | Lit | Écrit à `dst` |
|---|---|---|
| 0x11d710 | 1 vec4 | 4 mots retournés |
| 0x11d728 | 1 vec4 + en-tête r2 | `[m3][r2][m2][m1][m0]` : le début d'une commande 0x2C1 |
| 0x11d744 / 0x11d76c | 2 vec4 | idem, sans / avec en-tête |
| 0x11d798 / 0x11d7d0 | 3 vec4 (matrice 3×4 en lignes) | idem, sans / avec en-tête |
| 0x11d80c / 0x11d854 | 4 vec4 (matrice 4×4) | idem, sans / avec en-tête |
| 0x1145d8 | `__aeabi_memcpy` (non aligné) | copie brute de blocs préparés |
| 0x4b3d3c `(reg, n, valeurs)` | n mots | Écrit une suite de registres consécutifs (`reg`, `reg+1`…) en tranches de 128 : `[v0][0x800F0000 \| reg \| (k−1)<<20][v1…]`. |
| 0x5409fc `(adresse, taille)` | | Appel de sous-liste : écrit 0x238-0x23C = `(taille>>3, 0, phys(adresse)>>3, phys(retour)>>3, 1)` par 0x4b3d3c. Le GPU exécute alors un tampon de commandes externe et revient. S+ : `nngxAdd…Subroutine`/jump. Un seul appelant (0x1b1250, dans 0x1b0cb0, §5). |

## 1. Tables de correspondance (éclairage 0x1C8, brouillard 0xE8) : 0x174214 (V)

`0x174214(lut, emplacement)` est `ActivateLookupTable` (S+).

- **Cache** : 0x6b2fac contient 24 pointeurs, un par emplacement. Si `cache[emplacement] == lut`, la fonction ne fait rien. Sinon elle met le cache à jour et écrit :
  - si emplacement == 0x17 (brouillard) : `[0][0x000F00E6]` (index de la table de brouillard = 0) ;
  - sinon : `[sel << 8][0x000F01C5]`, où `sel = emplacement + (emplacement >= 7 ? 1 : 0)` (identifiant PICA de la table : 0-6, puis 8-23, le 7 n'existant pas) et l'index de départ vaut 0 ;
  - puis **`memcpy(liste, lut+0x10 + *(lut+0x10), *(lut+0xc))`**.
- `lut` est une ressource `ResLookupTable` du fichier CGFX (S+) :
  - +0x0c = taille du bloc de commandes en octets ;
  - +0x10 = décalage relatif (depuis +0x10) vers ce bloc.
- **Les données 0x1C8-0x1CF (ou 0xE8-0xEF) sont donc un bloc de commandes déjà présent dans le fichier.** Le code ne les calcule pas.
- 0x1742dc vide tout le cache (24 entrées). 0x1742ac retire une table du cache.
- Appelants :
  - 0x17085c (brouillard, §2) ;
  - 0x170e30, 0x17102c et 0x171180 (tables de l'éclairage du fragment, dans la boucle des lumières, §3) ;
  - 0x18bf0c, 0x18bfa4, 0x18c03c, 0x18c0cc, 0x18c168 et 0x18c1fc (tables du matériau, §3).

**En natif** : remplacer 0x174214. La clé est le pointeur `lut`. Au premier passage, décoder le bloc (une suite de commandes 0x1C8 : 256 entrées, valeur sur 12 bits et différence sur 12 bits, format PICA) en une texture 1D, et la garder. À chaque appel, lier simplement la texture à l'emplacement `sel`. Le cache existant garantit déjà que l'appel n'arrive que si la table change.

Le poids vient de là : une table fait environ 260 mots. Les matériaux qui alternent forcent son renvoi à chaque changement de matériau.

## 2. Brouillard : 0x17085c `(ctx)` (V)

`ctx` est le contexte de rendu (`RenderContext`, S+). Ses champs sont au §3.

- `fog = ctx+0xa0`. Ce champ est posé par 0x17183c depuis la table des brouillards de la scène : `ctx+0xc0[ shader+0x29c ]`, avec `shader = ctx+0x20 (matériau) → +0x54`. Si `fog == 0`, la fonction ne fait rien.
- `rf = fog+8` est la ressource du brouillard :
  - `rf+0xb4…+0xc0` : couleur (4 f32, bornés à 0..1, × 255 + 0.5) ;
  - `rf+0x18` bit 1 : inversion de la profondeur ;
  - `rf+0xc4` : décalage relatif vers sa table.
- Commandes émises :
  ```
  [(rf+0x18 & 2) ? 0x10000 : 0][0x000400E0]     TEXENV_UPDATE_BUFFER, octet 2 : sens z du brouillard
  [R | G<<8 | B<<16][0x000F00E1]                 FOG_COLOR (alpha ignorée)
  ```
- Puis `0x174214(table, 0x17)` : table du brouillard, comme au §1.
- Déclenché par 0x17183c quand `ctx+0xec` (brouillard sale) est levé, ou quand le brouillard choisi change.

**En natif** : couleur du brouillard, mode (inversion), table 1D. C'est un état par matériau qui change rarement.

## 3. Chaîne de dessin d'un maillage et activation (V, noms S+)

```
0x1b3888  rendu d'une file (passe)                      → pour chaque élément : 0x169990
0x169990  MeshRenderer::RenderMesh(renderer, mesh, model)
            ctx = renderer+8 (RenderContext)
            ctx+0x20 = model+0x164[ mesh+0x1c ]          (matériau actif)
            0x170a88(ctx, 0)                             ActivateMaterial
            0x171794(ctx, mesh)                          blocs du maillage
            0x169f5c(renderer, model, shape, mesh+0x28)  dessin de la forme (§4)
            memcpy(liste, *(mesh+0x68), *(mesh+0x6c))    bloc de fin du maillage
```

La forme vient de `ResModel` : `res = model+8`, `+0xc8` est le décalage relatif de la table des formes, indexée par `mesh+0x18`.
- Type `0x10000001` (`SeparateDataShape`) : le chemin ci-dessus.
- Type `0x10000002` (particules) : `0x1716f4` puis `0x169c28`, non décodés.

### 3.1 `0x170a88(ctx, flag)` : ActivateMaterial (V)

- Ne fait rien si `ctx+0x20 == ctx+0x24` (même matériau que le précédent).
- Si `!(ctx+0x14 & 2)` :
  - `shader = matériau+0x20` ;
  - si `shader != (ctx+8)+8` (le shader courant) : en cas de changement de mode géométrie, `0x1709fc(ctx+0x100, 0x1015)` (invalide des caches d'état) et `ctx+0xe8/+0xe9/+0xeb/+0xec = 1` (sales) ; puis **`0x172270(ctx+8, shader)`** (§3.2).
- **`0x17183c(ctx)`** : lumières, brouillard (§3.3).
- Si `!(ctx+0x14 & 1)` : **activateur virtuel** `act = *(matériau+0x1c)+0x1ec`, puis `act->vfunc[3](act ou flag, ctx, matériau)`. Ce sont les fonctions `0x177600` et `0x17c398` (§3.4).
- Enfin, les drapeaux sales sont remis à 0 et `ctx+0x24 = ctx+0x20`.

### 3.2 `0x172270(sh = ctx+8, prog)` : activation du shader (V)

- `bin = prog+0x84` est le décalage relatif vers le binaire de shader (S+ : `ResShaderBinary`, prêt au chargement). `prog+0x10` est l'index du shader de géométrie (< 0 : aucun).
- Si le mode géométrie diffère de celui du shader courant : `0x18c420(aGéométrie)` écrit le mode, puis `sh+0x10 = 2 ou 1` et `sh+0x44 = aGéométrie`.
- **Si le binaire diffère :** `memcpy(liste, *(bin+0x38), *(bin+0x3c))`. C'est le **bloc du programme** : code (0x2CB/0x2CC), descripteurs (0x2D5/0x2D6), et leurs pendants géométrie. Il est construit une fois et recopié à chaque changement de binaire. C'est la source des renvois de 0x2CC/0x2D6.
- **Toujours :** `memcpy(liste, *(prog+0x7c), *(prog+0x80))`. Bloc propre au programme, construit une fois : point d'entrée, table des sorties, permutation des entrées (S+).
- Uniformes booléens du shader de sommets : `d = bin+0x40[prog+0xc] → +8` ; table `d+0x18` (relative), `d+0x1c` entrées de 20 octets `{u16 type, u16 index, u32 valeur}`. Pour chaque `type == 0` : `masque |= (valeur & 1) << index`. Puis `sh+0x14 |= masque`. Même calcul pour la géométrie (`prog+0x10`) dans `sh+0x2c`.
- `sh+8 = prog`.
- État `sh` (constructeur 0x172434) :
  - `+0x14` = booléens du shader de sommets (initialisés à 0x7fff0000), suivis de `[0x804F02B0]` (+0x18) puis i0-i3 (+0x1c…+0x28) ;
  - `+0x2c` = booléens de géométrie, suivis de `[0x804F0280]` ;
  - `+0x48` = `0x00010244` (0x244, octet 0).
  - Ces blocs sont émis par 0x11eb1c (§4.3).

**En natif** : un programme par binaire (clé `bin`), et des uniformes booléens tirés de la table des constantes du DVLE et de `sh+0x14`. Les bits 1 et 2 (peau), 7 et 8 (`mesh+0x2c`, §3.5) sont modifiés pendant le dessin.

### 3.3 `0x17183c(ctx)` : lumières et brouillard (V en partie)

- Si `!(ctx+0x14 & 4)` :
  - `r7 = matériau+0x54` (paramètres d'ombrage, S) ;
  - `0x1764b8(ctx+0x40, r7+0x298)` : environnement de lumière, non décodé.
- Si `ctx+0xe8` (lumières sales) :
  - les globaux 0x5f2310 = 0 et 0x5f2314/18/1c = 0xff ;
  - pour `i < ctx+0xe4` : `0x170d58(ctx, i, ctx+0x64[i])`, l'activation de la lumière de fragment i. Elle appelle 0x174214 pour les tables des projecteurs et d'atténuation (§1). Non décodée en détail ;
  - puis `[0x5f2314 | 0x5f2318<<8 | 0x5f231c<<24][0x000B01C4]` (LIGHTING_CONFIG1, octets 0, 1 et 3 : bits d'activation des tables par lumière).
- Si `ctx+0xeb` : `0x17155c(ctx)`, non décodé.
- Brouillard : `f = ctx+0xc0[ r7+0x29c ]` ; s'il diffère de `ctx+0xa0`, alors `ctx+0xa0 = f` et `ctx+0xec = 1`. Si `ctx+0xec` est levé : `0x17085c` (§2).

### 3.4 Activateurs de matériau `0x177600` / `0x17c398` (V pour la structure, S+ pour les noms)

Ce sont les deux fonctions les plus grosses (3,4 Ko chacune, quasi identiques ; tables 0x59be3c et 0x59bf7c ; S+ : `MaterialActivator` et `SimpleMaterialActivator`, présentes dans la RTTI de X/Y). Arguments : `ctx` (dans la pile), `m` = matériau (r2).

**Principe.** Le matériau `m` pointe vers des sous-ressources (`m+0x28…+0x4c`). Chacune porte un identifiant ou une version à un décalage fixe (`+0x2a0…+0x2d0`). Le contexte garde le dernier identifiant envoyé dans `ctx+0x100…+0x128`. Un bloc n'est renvoyé **que si son identifiant change** (ou si l'identifiant vaut 0, ou si un drapeau « sale » de `ctx` est levé). `0x1709fc(ctx+0x100, masque)` met des emplacements à −1 pour forcer le renvoi.

| Cache ctx | Source (m) → identifiant | Ce qui est émis (V) | Rôle (S+) |
|---|---|---|---|
| +0x100 | `m+0x28` → `+0x2a4` | `0x18b214(sh, m+0x28)` | paramètres du shader (uniformes du matériau), non décodé |
| +0x104 | `m+0x2c` → `+0x2a0` (drapeaux `+0x18`) | `b = (+0x18 & 1) && ctx+0x40+0xa4 (nombre de lumières)`. `sh+0x14` bit 15 = b. Puis `[!b][0x000101C6]` et `[b][0x0001008F]`. Brouillard : `[mode][0x000100E0]`, mode = 5 si `+0x18 & 0x10` et brouillard présent, sinon 0. Bits 5 et 6 de `sh+0x14` selon `+0x18 & 4/8`. | activation de l'éclairage du fragment, mode brouillard |
| +0x10c | `m+0x34` → `+0x2b8` ; données à `+0x104` | Décalage de profondeur : modèle 0x6c1d9c = `[enable][0x0001006D] [échelle f24][0x8017004D] [décalage f24]…`. Le décalage (`+0x104+8`) est mis à l'échelle selon le format de profondeur (global 0x5f22fc : 0x81a5 = 16 bits → 2^-16, sinon × 128 × 2^-24). Émis par 0x11eb1c. Puis `[*(+0x104+0xc)][*(+0x104+0x10)]`, une commande de 2 mots prise dans la ressource. | décalage de polygone, profondeur |
| +0x110 | `m+0x38` → `+0x2a8` | `0x18b7c0(ctx, sh)`, non décodé | coordonnées de texture (matrices de texture, S) |
| +0x114 | `m+0x3c` → `+0x2b0` | Pour i < 3 : `tm = rel(m+0x3c + 0x274 + 4i)`. S'il existe : **`memcpy(liste, tm+0x10, *(tm+0x48))`** (bloc préparé de l'unité i : 0x81-0x9E), et `masque \|= 1<<i`. Puis `[masque \| 0x10000][0x00050080]`. | **textures** : blocs complets dans la ressource |
| +0x118 | `m+0x40` → `+0x2bc` ; config = `rel(+0x288) + 0x10` | `[CONFIG1 calculé][0x000F01C3]`. Le mot est construit depuis les octets `+4, +8, +0xc, +0x10` et les drapeaux `+0x0/+0x14` de la config : sélecteurs Fresnel et bump, `+0x14` → bit 30, bit 31 = 1, bit 10. Puis `[CONFIG1 haut][0x000401C4]`, et pour chaque lumière i : `[type][0x00010149 + 16i]` (LIGHTi_CONFIG). type = bit 0 directionnelle ou ombre selon `0x5f2310`, bit 1 = `lumière+0x18 & 2`, bit 2 = `!(config & 8)`, bit 3 = `!(config & 0x10)`. | éclairage du fragment |
| +0x11c | `m+0x44` → `+0x2c0` | `0x18be20(config, rel(rel(+0x288)+0x28))` : entrées des tables (0x1D0 ABS, 0x1D1 SELECT, 0x1D2 SCALE), puis 0x174214 pour chaque table utilisée (§1) | tables de l'éclairage |
| +0x108 | `m+0x30` → `+0x2b4` | `0x18ae38(ctx+0x40, sh, m+0x30+0x24, …)` : 0x1C0 (ambiante), 0x1C2 (nombre de lumières), 0x1D9 (permutation), non décodé | environnement de lumière |
| +0x120 | `m+0x48` → `+0x2c8` ; `f = rel(+0x288)` | `0x11eb1c(liste, f+0xdc)` : bloc préparé du **TEV**, construit à partir de la ressource. Puis pour chaque étage (entrées de 28 octets à `f+0x2c`, type = octet bas, 11 cas) : la couleur constante est réécrite depuis `ctx+0x40+0xc0/+0xc4…` (couleurs de la scène), puis renvoyée par 0x11eb1c. | **TEV** (étages et couleurs constantes) |
| +0x124/+0x128 | `m+0x4c` (S) → `+0x2d0` ; opérations `+0x118` | Masque couleur et profondeur `[…][0x00020107]`, modèle 0x5f22c8 (0x28 octets, test de profondeur, mélange et accès au tampon d'image, S) recopié par `memcpy`, plus des commandes de la ressource (`+0xc/+0x10`). Les globaux 0x5f22a0-0x5f22a8 gardent l'état courant (écriture et test de profondeur). | opérations sur le fragment (mélange, profondeur, pochoir) |

**Préparé une fois (V)** : blocs des textures (`tm+0x10`), bloc du TEV (`f+0xdc`), tables (§1), commandes de 2 mots des ressources.
**Calculé à chaque activation** : les mots 0x1C3/0x1C4/0x149+, 0x08F/0x1C6/0x0E0, le décalage de profondeur, les couleurs constantes du TEV, le masque 0x080.

**En natif** : remplacer l'activateur par une lecture directe de la ressource. Ce sont les mêmes structures `Res*` du CGFX, déjà analysées par le jeu.
- Pour les textures, décoder le bloc `tm+0x10` une fois (adresse, taille, format, paramètres), avec le pointeur `tm` pour clé.
- Pour le TEV, décoder le bloc `f+0xdc` (étages 0xC0-0xFD) une fois.
- Garder la logique du cache `ctx+0x100…` : elle évite déjà les renvois.

Non décodés : 0x18b214, 0x18b7c0, 0x18ae38, 0x18be20 (en détail), et la fin des opérations sur le fragment.

### 3.5 `0x171794(ctx, mesh)` : blocs du maillage (V)

- Si `ctx+0x12` : `0x18c3f0`, qui recopie un bloc fixe (0x5f4290) de mode shader.
- `memcpy(liste, *(mesh+0x60), *(mesh+0x64))` : **bloc préparé du maillage** (S+ : configuration des tampons de sommets 0x200+, construite au chargement avec les adresses physiques).
- `memcpy(liste, mesh+0x30, 0x30)` : 12 mots **dans l'objet maillage** (S : une petite commande, par exemple un uniforme propre au maillage, que le jeu peut modifier).
- `mesh+0x2c` bit 0 et bit 1 donnent `sh+0x14` bits 7 (0x80) et 8 (0x100).

## 4. Matrices et uniformes (V)

### 4.1 `0x169f5c(renderer, model, shape, iPrim)` : dessin d'une forme

1. `[0x80000006][0x804F02C0]`, puis 0x11d8a0 (un vec4 retourné) depuis `shape+0x20` : **c6** = décalage de position de la forme (S+).
2. Si `model` est un `SkeletalModel` (vfunc 2 = info de type, comparée en chaîne à 0x5f2400), on garde `skel = model`.
3. Si `ctx+0x1c != model` : `0x1719e0(ctx, skel)` ou `0x171470(ctx, model)` (§4.2), puis `ctx+0x1c = model`.
4. Pour chaque jeu de primitives `ps` (table relative `shape+0x30`, nombre `shape+0x2c`) :
   - si `skel` et `ps+0` (nombre d'os) != 0 : `0x169a98` (palette, §4.2) ;
   - sinon : `sh+0x14 &= ~6`, puis `[0x80000019][0x000F02C0]` et 0x11d7d0 (en-tête `0x00BF02C1`) depuis `model+0x8c` : **c25-c27 = matrice monde 3×4** ;
   - `prim = ps+0x10 [iPrim]` (tables relatives), puis `0x170b84(ctx, prim)` (§4.3).

### 4.2 Matrices de modèle et palette d'os

- **0x169a98 (palette)** :
  - `mode = ps+8` : si `mode != 2`, alors `sh+0x14` bit 1 = 1 et bit 2 = 0 ; si `mode == 2` (peau lisse, S+), l'inverse ;
  - matrices de `skel+0x220` (le squelette) : vfunc 0x1c = matrices monde, vfunc 0x24 = matrices de peau (S+) ;
  - pour i < n (n = r3) : `os = ps+4[i]` (table relative) ;
  - la source est la matrice de peau, sauf si le drapeau 0x200 de l'os (`squelette+0x1c → os*16 + 0x28 → +4`) est levé en mode rigide, auquel cas c'est la matrice monde ;
  - elles sont écrites en **c25 + 3i** (3 vec4 chacune) : un seul en-tête `[0x80000019][0x000F02C0][…][0x2C1, 3n−1 suppl.]`.
- **0x1719e0 (modèle à squelette)** :
  - c0-c2 = `model+0x8c` (monde) si le bit 0 de `res+0x28` (via `model+0x220 → +8`) vaut 0 ; sinon une identité (0x6c1d2c) ;
  - puis `[0x80000003][0x2C0]` et c3-c5 = `model+0x1bc` (S : matrice normale ou vue-modèle).
- **0x171470 (modèle simple)** : même principe, à partir de l'identité 0x6c1d2c. Non lu en détail.
- **0x170c74(ctx, caméra, forcer) — caméra.** Si la caméra change (`ctx+0x28`) :
  - c90-c92 = `caméra+0x148` (vue 3×4) ;
  - c86-c89 = `caméra+0x1a8` (projection 4×4) ;
  - les mêmes en uniformes de géométrie (0x290/0x291).
  - Appelants : 0x137d00, 0x138000 et 0x138218 (moteur), 0x1af33c, 0x1b0d80 et 0x1b51cc.

### 4.3 `0x170b84(ctx, prim)` : uniformes et blocs de la primitive (V)

- `0x11eb1c(liste, sh+0x14)` : bloc des uniformes booléens et entiers du shader de sommets, `[booléens][0x804F02B0][i0][i1][i2][i3]` (S+). Même chose pour la géométrie (`sh+0x2c`) s'il y en a une.
- Si `!(prim+0x10 & 1)` : `0x18005c(&prim, …)` (préparation paresseuse, S).
- Pour chaque flux d'indices `e` (table relative `prim+4`, nombre `prim+0`) dont l'octet `e+5` est non nul : `memcpy(liste, *(e+0x18), *(e+0x1c))`. C'est le **bloc de dessin préparé** : tampon d'indices, nombre et lancement (S+ : 0x227, 0x228, 0x22E, venus du CGFX ou construits au chargement).

**Bilan pour le natif.**
- Préparé une fois : le programme (`bin+0x38`), le bloc du programme (`prog+0x7c`), les blocs du maillage (`mesh+0x60`, `mesh+0x68`), les dessins (`e+0x18`) et les tables (`lut+0x10`).
- Change à chaque image : c6, c25+ (monde ou palette), c0-c5 (modèle), c86-c92 (caméra), les booléens `sh+0x14`, la couleur du brouillard et le choix des tables.

## 5. Passe de scène et sous-liste (V)

`0x1b0cb0` (appelée par 0x1b40cc) est la passe 3D de l'écran du haut (S+).
1. Elle prépare l'état et la caméra : `0x170808`, puis **`0x170c74(ctx, caméra, …)`** (§4.2).
2. Rendu en sous-passes : `0x1b28e4` (tri ou préparation de la file, S), puis `0x1b3888` (rendu de la file, 7 appelants), puis `0x4206a4`/`0x420978`/`0x17ed3c` (S : ombres ou particules).
3. **Enregistrement dans une liste à part** : `nngxBindCmdlist(*(0x5eeb00))`, `nngxClearCmdlist`, `nngxStartCmdlistSave` (0x541204). Puis les passes 3D : plusieurs couples `0x1b28e4`/`0x1b3888`, les effets `0x306ba8`/`0x30c37c`/`0x306c64`, et `0x3dfdfc`, `0x4515bc`, `0x210ed4`. Enfin `nngxStopCmdlistSave` (0x5412e8).
4. Retour à la liste principale. Deux blocs de 8 octets passent par `nngxAdd3DCommand` (0x1b1210/0x1b1220). Puis `nngxBindCmdlist`, et **`0x5409fc(sl+0x4c, sl+0x28)`** : la passe enregistrée est **appelée comme sous-liste** (0x238-0x23C). La boucle (`ldrb [sl]`, retour en 0x1b0f1c) refait l'enregistrement et l'appel, par exemple pour chaque œil (S).

Conséquence pour ctrnative : les ~8 000 mots de la 3D sont **réécrits chaque image** dans la liste à part, puis exécutés par saut. Contrairement aux layouts, il n'y a pas de cache de liste enregistrée réutilisé d'une image à l'autre. Remplacer les écrivains ci-dessus (§1 à §4) par des appels natifs supprime ces mots à la source. Le saut 0x238 ne porte alors plus que ce qui reste.

`0x1b3888(…)` parcourt une file d'éléments. Pour chacun : `0x170748`, un appel virtuel, puis `0x169990` (§3), puis un autre appel virtuel. Elle est non décodée au-delà.

`0x1b4ff4` (appelée par 0x137ed4 et 0x1380d8, le moteur du jeu) écrit `[0x80000050][0x804F02C0]` (c80, un vec4 depuis 0x11d710), puis `0x170c74` (caméra) et `0x170808`. Elle relit des globaux de scène (0x5e64c0, 0x5e64a8…). S : réglage de caméra ou d'environnement par scène.

## Limites

- Aucune exécution. Les champs et les mots sont lus dans le code (V). Les rôles des sous-ressources du matériau sont S+ : ils viennent de la structure de `nw::gfx`, pas de noms présents dans NSMB2.
- Je n'ai pas décodé le contenu des blocs préparés (programme, textures, TEV, dessin) octet par octet. Ce sont des suites de commandes PICA ordinaires, à décoder une fois au chargement.
- Non décodés :
  - 0x18b214, 0x18b7c0, 0x18ae38, 0x18be20 ;
  - 0x170d58 (lumière de fragment), 0x17155c, 0x1764b8 ;
  - 0x18005c, 0x11eb1c (en détail) ;
  - les particules (0x1716f4, 0x169c28), 0x1b28e4 et les effets 0x306ba8.
