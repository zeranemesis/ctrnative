# NSMB2 : le dessinateur `nw::lyt` champ par champ (2026-11-23)

Suite de [NSMB2-RENDU.md](NSMB2-RENDU.md). Analyse statique du code de NSMB2 (base 0x100000) et des shaders de sa RomFS, sans rien lancer.

Légende :
- **V** : lu dans le code ou dans le shader désassemblé. C'est ce que fait la machine, sans interprétation.
- **S+** : interprétation avec de bons indices (nom ou rôle).
- **S** : supposé.

Les offsets sont en hexadécimal. « D » désigne l'objet dessinateur (`nw::lyt::Drawer`, dérivé de `nw::font::RectDrawer`, S+). Dans le jeu, c'est le singleton 0x47de8c + 0x360.

**Point clé pour une version native (V).** Chaque quadrilatère du layout se résume à un enregistrement de 4 flottants. Cet enregistrement indexe trois tableaux d'uniformes : la matrice et les couleurs (B), la taille, la position et les UV (C). Le shader reconstruit les 4 coins à partir d'un tampon de sommets fixe. Refaire le dessin en natif revient donc à :
1. pour chaque quadrilatère : matrice 3×4, taille, point de base, 4 couleurs (ou une alpha), jusqu'à 3 jeux d'UV aux 4 coins ;
2. pour chaque matériau : TEV en mode 0, 1 ou 2 (formules ci-dessous), mélange, test alpha, jusqu'à 3 textures avec leurs paramètres d'échantillonnage.

Aucune commande PICA n'est nécessaire pour cela.

## 0. Récapitulatif des décalages (détails dans les sections suivantes)

**Uniformes du shader** (ordre mémoire, voir §1) :

| Registre | Contenu |
|---|---|
| c0-c3 | projection 4×4, en lignes |
| c4 | parallaxe : `(parallax, dLevel, addDist, 0)` |
| c6+k | enregistrement du quadrilatère k : `(iMtx, iRect, iUV, couleurOuAlpha)` |
| B = c32+j | matrice 3×4 du panneau (3 vec4), puis 4 couleurs HG, HD, BG, BD en flottants 0..1, l'alpha déjà multipliée |
| C = c64+j | rectangle `(largeur, hauteur, base.x, base.y)`, puis UV : 3 vec4 `(uHG, vHG, uBD, vBD)` (textures 0-2), puis 3 vec4 `(uBG, vBG, uHD, vHD)` |
| i0.x | 0 (UV par défaut) ou 6 |

**Matériau** (`mat = Pane+0x138`) :

| Offset | Type | Rôle |
|---|---|---|
| +0x10 | u32 RGBA | couleur « noir » (étage 3 en mode 1, couleur du tampon TEV en mode 2) |
| +0x14 | u32 RGBA | couleur « blanc » (constante de l'étage 5 en mode 0, de l'étage 4 en modes 1 et 2) |
| +0x28 | u32 RGBA | couleur d'interpolation des 2 textures (mode 2, son alpha est utilisée) |
| +0x2c | u32 | bit 0x200 = test alpha personnalisé, 0x400 = mélange personnalisé. Les bits bas servent à 0x48f894 pour trouver `{u8 func, f32 ref}`. |
| +0x30 | u32 | bits 0-1 = nombre de textures (donne aussi le mode TEV) ; bits 4-5 = nombre de générations d'UV ; bits 0x1c0 = TEV personnalisé |
| +0x34 | pointeur | textures, 32 octets chacune : +4 adresse physique (octets), +8/+0xa largeur/hauteur utiles (u16), +0xc/+0xe largeur/hauteur allouées (u16), +0x14 paramètres = registre TEXUNIT_PARAM (filtre, répétition), +0x18 dimensions (largeur << 16 \| hauteur), +0x1c format PICA |
| +0x4d | u8 | bit 1 = ne rien régler ; bit 2 = cache des UV valide |

Les tables de transformation des textures (SRT, 20 octets : `tx, ty, rotation, sx, sy`) et des générations d'UV (octet 1 = jeu de coordonnées source) sont trouvées par 0x48f7cc et 0x48f86c, à partir de +0x2c et +0x34. Je n'ai pas décodé leur calcul d'adresse.

**Panneau Picture** (P) :

| Offset | Type | Rôle |
|---|---|---|
| +0x48 / +0x4c | f32 | largeur / hauteur |
| +0x80 | 3×4 f32 | matrice globale (modèle-vue), en lignes |
| +0xb5 | u8 | alpha globale |
| +0xb7 | u8 | bit 0 = visible (racine, voir 0x1930d8) |
| +0xd2 | u8 | UV déjà calculées |
| +0xd4 | u32 | nombre de vec4 d'UV (0 ou 6) |
| +0xd8 | 6 vec4 | UV calculées |
| +0x138 | pointeur | matériau |
| +0x13c | 4 × u32 RGBA | couleurs des sommets HG, HD, BG, BD |
| +0x150 | 32 octets par jeu | coordonnées de texture brutes HG, HD, BG, BD (u, v) |

Le point de base est calculé par 0x48dfd4(P) à partir de l'origine du panneau. Je n'ai pas décodé ce calcul.

---

## 1. Le shader de sommets : `nwfont_RectDrawerShader.shbin` (V)

Il vient de la RomFS (964 octets, un DVLE, 104 instructions, 35 descripteurs). Le constructeur 0x1a9954 l'importe lui-même : code par 0x2CB/0x2CC, descripteurs par 0x2D5/0x2D6, constantes du DVLE. Les autres shaders de la RomFS ne servent pas ici :
- `nwlyt_PaneShader.shbin` : uniformes nommés `uProjection`, `uModelView`… ;
- `nwlyt_ConstColorShader`, `nwfont_TextWriterShader`.

Ce shader est le seul à avoir une entrée unique `vVtxData` et la constante c5 = (0, 1, 2, 3). Le code du dessinateur écrit justement c0-c4, c6+, c32+ et c64+.

**Ordre des composantes (V).** Les uniformes sont envoyés en float32 par 0x2C1, et le PICA range les 4 mots dans l'ordre w, z, y, x. Le shader les relit toujours avec `.wzyx`. Dans la suite, `m0..m3` désignent les 4 flottants **dans l'ordre de la mémoire du CPU** : ce sont directement les valeurs à fournir en natif.

| Registre | Contenu (ordre mémoire) | Source |
|---|---|---|
| c0-c3 | matrice de projection 4×4, en lignes | D+0x30…+0x6f, copiée de `drawInfo+4` (16 flottants) par 0x1aa954 |
| c4 | `(parallax, dLevel, addDist, 0)` | D+0x74…+0x80, posé par 0x1a98c8 `SetParallax` |
| c5 | `(0, 1, 2, 3)` | constante du DVLE |
| c6 + k | enregistrement A[k] du quadrilatère k = `(iMtx, iRect, iUV, colorOrAlpha)` | tableau A, D+0x94 (D+0x668 pointe dessus) |
| c32 + j | tableau B : matrices (3 lignes) et couleurs (4 vec4) | D+0x244 (pointeur D+0x66c) |
| c64 + j | tableau C : rectangle (1 vec4) et UV (6 vec4) | D+0x454 (pointeur D+0x670) |
| i0 | `x` = nombre de vec4 d'UV (0 ou 6), init 0, pas 0 | D+0x69c |

Entrée (V) : `v0 = (iQuad, coin, x, y)`, avec x ∈ {0, 1} et y ∈ {0, −1} (voir §2).

Calcul du shader (V, transcrit du désassemblage) :
```
A = c6[v0.x]                      // (iMtx, iRect, iUV, cA)
R = C[iRect]                      // (w, h, baseX, baseY)
p = (x*w + baseX, y*h + baseY, 0, 1)        // y vaut 0 ou -1 : le quad descend
q.xyz = B[iMtx+0..2] · p          // matrice 3×4 en lignes (modèle-vue), q.w = 1
// parallaxe stéréo :
d = |B[iMtx+2].m3| + addDist      // |tz| + addDist
si parallax != 0 et d != 0 :  q.x += parallax * (d - dLevel) / d
o0 = Proj(c0..c3) · q
// couleur :
si cA <= 1 : o1 = (1, 1, 1, cA)                       // les 4 couleurs sont blanches, cA = alpha globale
sinon      : o1 = B[cA + coin]  où coin = 0 haut-gauche, 1 haut-droite, 2 bas-gauche, 3 bas-droite
             // ordre mémoire R, G, B, A en flottants 0..1, alpha déjà multipliée
// UV, 3 jeux (o2, o3, o4) :
par défaut : HG (0,1), HD (1,1), BG (0,0), BD (1,0)
si i0.x >= 1 : pour t = 0..2 : U = C[iUV + t] = (uHG, vHG, uBD, vBD)
               HG=(m0,m1)  HD=(m2,m1)  BG=(m0,m3)  BD=(m2,m3)
si i0.x >= 4 : pour t = 0..2 : W = C[iUV + 3 + t] = (uBG, vBG, uHD, vHD)
               HD=(W.m2,W.m3)  BG=(W.m0,W.m1)        // coins quelconques (rotation)
```
Le jeu écrit toujours i0 = 0 (sans texture) ou 6 (les 4 coins explicites).

Sorties : o0 = position, o1 = couleur, o2, o3, o4 = coordonnées de texture 0, 1, 2 (masque xy).

## 2. Tampon de sommets fixe (V)

Le constructeur (0x1a9954) remplit 0x9c0 octets (le tampon passé en argument, D+0x67c), soit 26 quadrilatères × 6 sommets × vec4 float.

Pour le quadrilatère i et le coin c pris dans la table 0x588798 = `[1, 0, 3, 3, 0, 2]`, le sommet vaut `(i, c, (c & 1) ? 1 : 0, (c >= 2) ? -1 : 0)`. Cela donne deux triangles : (HD, HG, BD) et (BD, HG, BG).

Le tampon est vidé du cache par 0x5415bc. Une seule entrée d'attribut de 4 flottants (registres 0x200+ issus du modèle 0x5889e8). Je n'ai pas décodé ces registres d'attributs mot à mot.

Indices : 16 bits, 6 par quadrilatère. Le tampon d'indices est à l'adresse D+0x680, mais je n'ai trouvé ni qui la pose ni son contenu (supposé 0, 1, 2…, **non vérifié**).

## 3. Champs du dessinateur D (V, sauf mention)

| Offset | Type | Rôle |
|---|---|---|
| +0x04 | constructeur de blocs `{base, taille, …, +0xc copie}` | Bloc de début : import du shader, constantes et modèle 0x5887b0 (0x140 octets). Rejoué par `DrawBegin(flag)` si `!(flag & 1)`. |
| +0x14 | idem | Bloc de fin : 0x1c octets construits sur la pile, puis 0x588988 (0x94 octets, contient 0x2BB/0x2BC, la permutation des entrées) et 0x588730 (0x28 octets). Rejoué par `DrawEnd(flag)` si `!(flag & 1)`. |
| +0x24 / +0x25 / +0x26 | u8 | Remplissage des tableaux A (enregistrements), B et C (vec4). |
| +0x27 | u8 | Remplissage du tableau D du texte (§9). |
| +0x28…+0x87 | bloc c0-c4 | `[0x80000000][0x000F02C0][m0][0x013F02C1][m1…m15][parallaxe ×4][0]`. |
| +0x74…+0x80 | 4 f32 | parallax, dLevel, addDist, 0. |
| +0x88 | bloc A | `[0x80000006][0x000F02C0][premier mot][en-tête 0x2C1]`, données à partir de +0x94 (26 vec4 au plus). |
| +0x238 | bloc B | `[0x80000020][0x000F02C0]…`, données à partir de +0x244. |
| +0x278 | bloc D (texte) | `[0x80000023][0x000F02C0]…` |
| +0x448 | bloc C | `[0x80000040][0x000F02C0]…`, données à partir de +0x454. |
| +0x658…+0x667 | 4 f32 | Enregistrement en cours : `iMtx` (+0x658), `iRect` (+0x65c), `iUV` (+0x660), `colorOrAlpha` (+0x664). |
| +0x668 / +0x66c / +0x670 | pointeurs | Données des tableaux A, B et C. |
| +0x674 | pointeur | Données du texte (bloc D). |
| +0x678 / +0x67c | pointeurs | Tampon des blocs de début/fin, tampon de sommets. |
| +0x680 | adresse | Tampon d'indices. |
| +0x684…+0x68c | | Suivi des listes de texte (§9). |
| +0x690 | u8 | Drapeau du texte. |
| +0x694, +0x698 | | Remis à 0 par `DrawBegin`. |
| +0x69c | u32 | Valeur de i0 (nombre de vec4 d'UV du lot). |
| +0x6a0 | u8 | Cache « mélange par défaut actif ». |
| +0x6a1 | u8 | Cache « test alpha par défaut actif ». |
| +0x6a2 | u8 | Cache du mode TEV : 0, 1, 2, 3 ou 5 ; 6 = inconnu, après `DrawBegin`. |

`colorOrAlpha` : si les 4 couleurs de sommet sont blanches (0xFFFFFFFF), il vaut l'alpha globale/255, entre 0 et 1. Sinon, il vaut l'index dans B du premier des 4 vec4 de couleur. Cet index est toujours au moins 3, car la matrice du même quadrilatère est ajoutée avant. C'est ce qui permet au shader de les distinguer par `cA <= 1`.

## 4. Remplissage par `Picture::DrawSelf` 0x48f398 (V ; nom S+)

Arguments : r0 = panneau P, r2 = D.

1. `0x191c4c(D, P+0x138 matériau)` : `SetupGraphics` du matériau (§6).
2. `0x1921d8(D, mat, 1)` : textures (§7).
3. Si `P+0xd2 == 0` ou si le bit 2 de `mat+0x4d` vaut 0 : `n = 0x48e060(D, mat, P+0x150, P+0xd8)` calcule les UV (§5), puis `P+0xd4 = n`, `P+0xd2 = 1` et `mat+0x4d |= 4` (cache des UV valide).
4. Avec `n = P+0xd4` :
   - si `D+0x69c != n` : vidage (§8), puis `D+0x69c = n` ;
   - si n > 0 :
     - si n ≥ 2 : vidage (donc **un quadrilatère texturé par lot**) ;
     - `+0x660 = (float)D+0x26`, puis copie des n vec4 de `P+0xd8` dans C, et `D+0x26 += n`.
5. Matrice : copie de 3 vec4 (la matrice globale 3×4 du panneau, `P+0x80`, en lignes, par 0x11d370) dans `B[D+0x25]`. Puis `+0x658 = (float)index` et `D+0x25 += 3`.
6. Rectangle :
   - `C[D+0x26] = (P+0x48 largeur, P+0x4c hauteur, base.x, base.y)`. Le point de base est renvoyé par 0x48dfd4(P), calculé depuis l'origine du panneau (S+).
   - Puis `+0x65c = (float)index` et `D+0x26 += 1`.
7. Couleurs : `0x1924b8(D, P+0x13c, P+0xb5)`, avec 4 couleurs u8 RGBA (ordre HG, HD, BG, BD) et l'alpha globale u8 :
   - si les 4 couleurs valent 0xFFFFFFFF : `+0x664 = alpha/255` ;
   - sinon : `+0x664 = (float)D+0x25`, et `B[i..i+3]` reçoit les couleurs `(r, g, b, a·alpha/255)/255`. Une couleur blanche (absente) y vaut `(1, 1, 1, alpha/255)`. Enfin `D+0x25 += 4`.
8. Validation (même code que 0x192be4) :
   - `A[D+0x24] = +0x658…+0x664`, puis `D+0x24 += 1` ;
   - vidage si `D+0x24 + 1 > 26`, ou si `D+0x25 + 7 > 32`, ou si `D+0x26 + 7 > 32`.

`TextBox::DrawSelf` (0x48f724) passe par 0x1920dc (texte, §9). `Window::DrawSelf` (0x48e6a4) répète, pour le contenu et pour chacun des 4 coins (ou 8 morceaux) : matériau, textures, UV du coin, puis le bloc de quadrilatère suivant :
- 0x193050 `(D, src, n)` : identique à l'étape 4 ;
- 0x19310c `(D)` : matrice, comme l'étape 5, la source étant passée dans r1 ;
- 0x193144 `(D, &taille, &base)` : identique à l'étape 6 ;
- 0x1924b8 : couleurs ;
- 0x192be4 : validation.

Je n'ai pas décodé les 4 aides de coordonnées de coin (0x191850, 0x191950, 0x191a4c, 0x191b4c, selon le retournement du cadre).

## 5. UV : 0x48e060 `(D, mat, texCoords = P+0x150, sortie = P+0xd8)` → 0 ou 6 (V)

- `nGen = (mat+0x30 >> 4) & 3`, plafonné à 3. Si `nGen == 0`, la fonction renvoie 0 (UV par défaut du shader).
- Pour chaque génération i < nGen :
  - `src = octet 1 de l'entrée i` de la table renvoyée par 0x48f86c(mat). C'est l'index du jeu de coordonnées, qui vaut 32 octets : `P+0x150 + src*32` = HG(u,v), HD(u,v), BG(u,v), BD(u,v).
  - `tex = mat+0x34 + i*32`, avec `rw = u16 tex+8 / u16 tex+0xc` et `rh = u16 tex+0xa / u16 tex+0xe` (taille utile / taille allouée).
  - `srt = entrée i` de la table de 0x48f7cc(mat), 20 octets : `tx, ty, rotation, sx, sy`.
  - `(sin, cos) = SinCosIdx(rotation × 0.711111)`. 0.711111 = 256/360, donc des degrés convertis en unités de 1/256 de tour (S+).
  - Pour chaque point (u, v), avec `du = u − 0.5` et `dv = v − 0.5` (pivot (0.5, 0.5) lu en 0x5f4ad4) :
    ```
    u' = rw * (sx*cos*du − sy*sin*dv + 0.5 + tx)
    v' = 1 − rh * (sx*sin*du + sy*cos*dv + 0.5 + ty)
    ```
  - `sortie[i] = (u'HG, v'HG, u'BD, v'BD)` et `sortie[i+3] = (u'BG, v'BG, u'HD, v'HD)`.
- Renvoie 6. Un chemin rapide (rotation 0, échelle 1) donne le même résultat.

Lecture du sens de sin et cos : S+.

## 6. Matériau : `SetupGraphics` 0x191c4c `(D, mat)` (V ; nom S+)

Après chaque « vidage », la fonction écrit dans la liste courante (pointeur global 0x5f1dc0).

**0. Raccourci.** Si le bit 1 de `mat+0x4d` vaut 1 : rien n'est réglé (seul un vidage a lieu si le mode TEV en cache n'est ni 0 ni 1).

**1. Test alpha.**
- Si `mat+0x2c & 0x200` :
  - vidage, puis `D+0x6a1 = 0` ;
  - `[1][0x00000111]` : 0x111 avec un masque d'octets nul, sans effet sur un registre (S : barrière) ;
  - `(func u8, ref f32) = 0x48f894(mat)`, puis `[ (T[func] << 4) | (u8(ref*255 + 0.5) << 8) | 1 ][0x000F0104]`, avec T = 0x58e66c = `[0, 4, 5, 2, 3, 7, 6, 1]` qui traduit la comparaison lyt (NEVER, LESS, LEQUAL, EQUAL, NEQUAL, GEQUAL, GREATER, ALWAYS) vers le code PICA.
- Sinon, si `D+0x6a1 == 0` : vidage, `D+0x6a1 = 1`, puis `[1][0x111]` et `[0x61][0x000F0104]`. **Par défaut : test alpha activé, alpha > 0.**

**2. Mélange.**
- Si `mat+0x2c & 0x400` : vidage, `D+0x6a0 = 0`, `[1][0x111]` si pas encore écrit, puis 0x192394(D, mat), le mélange personnalisé (non décodé, relit 0x58e4cc et 0x58e618).
- Sinon, si `D+0x6a0 == 0` : vidage, `D+0x6a0 = 1`, `[1][0x111]` si besoin, puis les 16 octets de 0x58e4cc :
  ```
  [00e40100][000f0100]   COLOR_OPERATION : mode normal, mélange activé
  [76760000][000f0101]   BLEND_FUNC : couleur et alpha = ADD, src = SRC_ALPHA, dst = ONE_MINUS_SRC_ALPHA
  ```

**3. TEV.**
- Si `mat+0x30 & 0x1c0` : vidage, 0x192c50 (TEV personnalisé, non décodé, écrit 0x0E0 et 0x0FD), puis mode 5.
- Sinon, selon `n = mat+0x30 & 3` (nombre de textures) :

| n | Mode | Réglage complet (si le mode en cache diffère) | Mise à jour (même mode, couleur changée) | Formule |
|---|---|---|---|---|
| 0 | 0 | Vidage, puis les 24 octets de 0x5f4930 avec la couleur constante = `mat+0x14` : `[0ee00ee0][804f00f8][0][00010001][mat+0x14][0]`. Seul l'étage 5 (0xF8…0xFC) est écrit. | `[mat+0x14][0x000F00FB]` | `sortie = couleurSommet × mat+0x14` (RGB et alpha) |
| 1 | 1 | Vidage. Le modèle 0x5f4948 (0x48 octets) est copié avec 0x5f4958 = `mat+0x10` et 0x5f4970 = `mat+0x14` :<br>`[0e3e0e3e][804f00d8][00010010][00010001][mat+0x10][0]` (étage 3)<br>`[0f3e0f3e][804f00f0][0][00080008][mat+0x14][0]` (étage 4)<br>`[0ff00ff0][804f00f8][0][00010001][0][0]` (étage 5) | Si l'une change : `[mat+0x10][0x000F00DB]` et `[mat+0x14][0x000F00F3]` | `t = tex0`<br>`e3 = noir × (1 − t)` (noir = mat+0x10)<br>`e4 = blanc × t + e3` (blanc = mat+0x14, MULT_ADD)<br>`sortie = couleurSommet × e4`.<br>C'est l'interpolation noir/blanc de lyt. |
| 2 | 2 | 0x192934 : vidage (fait par l'appelant), puis le modèle 0x5f49f0 (0x58 octets) est copié avec 0x5f4a00 = `mat+0x28`, 0x5f4a18 = `mat+0x14`, 0x5f4a20 = `mat+0x10` :<br>`[0e430e43][804f00d8][00000200][00040004][mat+0x28][0]` (étage 3)<br>`[0fde0fde][804f00f0][0][00040004][mat+0x14][0]` (étage 4)<br>`[mat+0x10][000f00fd]` (couleur du tampon TEV)<br>`[0ff00ff0][804f00f8][0][00010001][0][0]` (étage 5)<br>`[0][000200e0]` (aucun étage n'écrit le tampon) | Si l'une des trois change : `[mat+0x28][0x000F00DB]`, `[mat+0x14][0x000F00F3]`, `[mat+0x10][0x000F00FD]` | `e3 = lerp(tex1, tex0, mat+0x28.alpha)` (INTERPOLATE, opérande 2 = alpha de la constante)<br>`e4 = lerp(noir, blanc, e3)`, avec noir = tampon = mat+0x10<br>`sortie = couleurSommet × e4` |
| 3 | 3 | 0x192a6c (non décodé : écrit 0x0CB, l'étage 1, et relit 0x5f4a20 et 0x5f4a48) | | |

Couleurs du matériau : `mat+0x10` = noir, `mat+0x14` = blanc, `mat+0x28` = couleur d'interpolation. Ce sont des mots u32 dont les octets en mémoire sont R, G, B, A, soit le format des couleurs constantes PICA. Les noms sont S+ (usage lyt) ; les rôles dans les formules sont V.

Les étages 0-2 ne sont jamais réglés ici. Ils doivent rester en mode « transmettre » (réglés par le bloc de début, S). Les étages 3 et 4 ne servent plus en mode 0, puisque l'étage 5 ne lit pas « précédent ».

## 7. Textures : 0x1921d8 `(D, mat, flag)` (V)

- Si `mat == 0` : les adresses du modèle sont mises à 0 (0x5f4998, 0x5f49b8, 0x5f49d8). Puis, si `flag`, 0x1aa150 écrit `[0][0x0002006F]`, `[00000000][00200080][0][0]` (écriture factice) et `[0x1000][0x000B0080]` : textures coupées.
- Sinon :
  - `n = min(mat+0x30 & 3, 3)`. Pour chaque unité i < n, l'entrée de texture `T = mat+0x34 + 32*i` donne :
    - `T+4` : adresse physique, divisée par 8 avant d'aller au registre 0x85, 0x95 ou 0x9D ;
    - `T+0x14` : paramètres (filtre, répétition) pour 0x83, 0x93 ou 0x9B ;
    - `T+0x18` : dimensions (largeur << 16 | hauteur) pour 0x82, 0x92 ou 0x9A ;
    - `T+0x1c` : format pour 0x8E, 0x96 ou 0x9E.
  - Changement : si l'adresse (`T+4 >> 3`) ou les paramètres (`T+0x14`) diffèrent du modèle 0x5f4990, la fonction met à jour le modèle (4 mots) et lève « changé ».
  - `masque = bits 0..n−1`. `aucunEnAttente = (D+0x24 == 0)`.
  - Si `aucunEnAttente` ou « changé » : vidage s'il y a des quadrilatères en attente, puis les 16 octets de 0x58e4cc+0x10 : `[0][00200080][0][0]` (écriture factice dans 0x80, masque nul).
  - Si `aucunEnAttente` : `[masque << 8][0x0002006F]` (sorties de coordonnées de texture actives), puis `[masque | 0x11000][0x000B0080]`, qui active les unités ; bit 12, plus bit 16 = vider le cache de textures.
  - Si `aucunEnAttente` ou « changé » : `[masque | 0x11000][0x00040080]`, soit l'octet 2 seul (vidage du cache de textures).
  - Si « changé » : copie de `32·n` octets du modèle 0x5f4990 (unité 0 : 0x82, 0x85, 0x8E, 0x83 ; unité 1 : 0x92, 0x95, 0x96, 0x93 ; unité 2 : 0x9A, 0x9D, 0x9E, 0x9B).

En natif, il faut : une texture par unité (adresse, taille, format), plus filtre et répétition (`T+0x14`, à décoder comme le registre TEXUNIT_PARAM de PICA).

## 8. Vidage d'un lot : 0x192788 `(D)` (V)

Avec nA = D+0x24, nB = D+0x25, nC = D+0x26 :

1. **Tableau A.** `D+0x90 = D+0x94` (le premier mot remonte), `D+0x94 = ((4·nA − 1) << 20) | 0x000F02C1`, `A[nA].m3 = 0`. Puis `memcpy4(liste, D+0x88, 16 + 16·nA)`. Cela émet :
   `[0x80000006][0x000F02C0] [A0.m0][en-tête 0x2C1, 4nA−1 suppl.] [A0.m1 … A(nA−1).m3] [0]`
2. Si nB > 0 : 0x1a98dc(D) fait la même chose sur le bloc +0x238 (c32), avec `16 + 16·nB` octets, puis `D+0x25 = 0`.
3. Si nC > 0 : même chose sur le bloc +0x448 (c64), puis `D+0x26 = 0`.
4. `[D+0x69c][0x000F02B1]` : uniforme entier i0 (x = nombre de vec4 d'UV).
5. `[0x80000000 | off][0x000F0227]` : tampon d'indices, 16 bits, à l'offset `off = 0x20` si `((D+0x680 + max(0, 2·6nA − 0x20)) & 0xFFF) >= 0xFE0`, sinon 0 (0x4944a8). Cela évite qu'une lecture d'indices franchisse une frontière de 4 Ko ; c'est une contournement du matériel (S+).
6. `[6·nA][0x000F0228]` : nombre de sommets.
7. Les 0x40 octets de 0x588758 (lancement de dessin indexé) :
   `[1][00010253] [0][00010245] [1][000f022e] [1][00010245] [1][000f0231] [0][0008025e] [0][0008025e] [0][00010253]`
8. `D+0x24 = 0`.

**Équivalent natif : un appel de dessin de nA quadrilatères avec les uniformes A, B, C et i0.**

## 9. Début, fin, projection, parallaxe ; texte (V sauf mention)

- **0x1aa924 `(D, drawInfo)`** = `DrawBegin(0)` (entrée virtuelle 3), puis 0x1aa954 :
  - copie de 16 flottants de `drawInfo+4` (la projection 4×4, S+) vers D+0x34 ;
  - `D+0x30 = D+0x34`, `D+0x2c = 0x013F02C1`, `D+0x84 = 0` ;
  - `memcpy4(liste, D+0x28, 0x60)` : `[0x80000000][0x2C0][m0][0x013F02C1][m1…m15][parallaxe ×4][0]`, ce qui charge c0-c4.
- **0x1aaf30 `DrawBegin(flag)`** (entrée virtuelle 3, table 0x59c328) :
  - remet à 0 +0x694, +0x698 et +0x69c, puis `+0x6a2 = 6` et `+0x6a0 = 1` ;
  - appelle 0x1921d8(D, 0, 0) ;
  - si `!(flag & 1)` : `[0][0x2006F]`, `[0][00200080][0][0]`, `[0x1000][0x000B0080]`, puis le bloc +0x04 rejoué par `nngxAdd3DCommand` (0x5407cc). Ce bloc contient l'import du shader ;
  - enfin `+0x684 = 0`.
- **0x1aaef4 `DrawEnd(flag)`** (entrée virtuelle 4) :
  - si D+0x24 : vidage 0x192788 ;
  - si `!(flag & 1)` : le bloc +0x14 rejoué par `nngxAdd3DCommand(base, taille, copie)`. Il se termine par 0x588730 :
    `[0][803f0232][3f000000][0][0][0]` (index d'attribut fixe 0x232, puis valeurs, S)
    `[1][000f0111]` (vidage du framebuffer)
    `[1][000f0110]` (invalidation du framebuffer)
- **0x1a98c8 `SetParallax(parallax, dLevel, addDist)`** : `D+0x74 = (s0, s1, s2, 0)`.
- **Dans le cache de layouts du moteur** (0x47d5f8, 0x47d258) :
  - l'enregistrement appelle `DrawBegin(1)` et `DrawEnd(1)` : sans les blocs +0x04 et +0x14 ;
  - le rejeu appelle `SetParallax`, puis 0x1aa924 (bloc de début + projection), puis `nngxUseSavedCmdlist`, puis `DrawEnd(0)` (bloc de fin).
- **Texte (S+, `nw::font`).**
  - 0x1a93a8 `(writer, ?, D)` rejoue une liste de caractères préparée (`DispStringBuffer`, writer+0x44) :
    - 0x1abad4 calcule une matrice ;
    - 0x1aae20 `(m)` l'écrit : `[0x80000023][0x000F02C0] [m[0]][0x00FF02C1] [m[1]…m[15]] [m[16] = bourrage]`. Ce sont 16 flottants en c35-c38 (V pour les mots, S pour le rôle de matrice) ;
    - 0x1aae8c écrit les couleurs de dégradé haut/bas (writer+0, +4), avec l'alpha writer+0x49 appliquée par la division par 255 (0x80808081) ;
    - enfin, copie de la liste préparée (writer+0x44 → +0x10 ou +0x14, taille +0x18 mots).
  - 0x1aa1a4 et 0x1aa9ac **construisent** cette liste quand le texte change. Ils utilisent le bloc D (+0x278, c35+, compte +0x27) et le modèle 0x588610 (0x60 octets, réglages du texte) :
    `[1][2b1] [1][111] [00e40100][100] [76760000][101] [61][104] [0ff00ff0][804f00f8][0][00010001][0][0] [100][2006f] [0][00200080][0][0] [00011001][000b0080]`
    Soit : i0 = 1, mélange alpha, test alpha > 0, étage 5 = sommet × précédent, unité de texture 0 seule. Ils vident ensuite comme au §8, mais vers la liste du texte (sans 0x2B1 par lot).
  - 0x1a98dc = vidage du tableau B seul (§8, étape 2).

## Limites

- Rien n'est exécuté. Les formules du shader et les mots émis sont lus dans le code (V), mais ne sont pas comparés à une capture.
- Non décodés :
  - le mélange personnalisé (0x192394), le TEV à 3 textures (0x192a6c) et le TEV personnalisé (0x192c50) ;
  - les aides de coins de `Window` ;
  - les registres d'attributs (0x5889e8, 0x5887b0, 0x588988) ;
  - le contenu et la pose du tampon d'indices D+0x680 ;
  - la construction détaillée des caractères (0x1aa1a4).
- Les noms de classes et méthodes (`Picture::DrawSelf`, `SetupGraphics`, `DrawBegin`…) sont S+ : ils viennent de la RTTI de X/Y, qui est une version plus récente.
- Scripts dans le dossier de notes partagé, `bibliotheques-outils/` : `picadis.py` (désassembleur de shader PICA), `dvlb.py` (uniformes et sorties d'un DVLB), `pdec.py` (décodeur de commandes), `dump.py`, `words.py`.
