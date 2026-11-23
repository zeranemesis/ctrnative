# NSMB2 : `nw::lyt` en natif — ce qui est décodé (2026-11-23)

Lu dans le code (désassemblage, `scratchpad/lyt/armdis.py`), pas encore observé en jeu.
Complète `NSMB2-RENDU.md`. But : remplacer ces fonctions par des appels natifs (`RENDU-NATIF.md`).

## Le dessinateur (r5 dans `Picture::DrawSelf`, `drawer` ailleurs)

Il **regroupe les rectangles par lots** et ne les envoie qu'au vidage 0x192788.
Le shader de 2D de `nw::lyt` dessine des rectangles décrits entièrement par des uniformes :
chaque rectangle renvoie, par numéros, à une matrice, à des coordonnées de texture et à des couleurs.

| Champ | Rôle |
|---|---|
| +0x24 (octet) | rectangles en attente dans le lot (vidage au-delà de 26) |
| +0x25 (octet) | prochaine ligne libre de matrices (3 lignes vec4 par matrice, 32 au plus) |
| +0x26 (octet) | prochaine ligne libre de coordonnées de texture (32 au plus) |
| +0x66c | tableau des matrices du lot (vec4 ×3 chacune) |
| +0x670 | tableau des coordonnées de texture (vec4) |
| +0x668 | tableau des rectangles : 4 flottants (n° matrice, n° couleurs/texcoords, …) |
| +0x658…+0x664 | le rectangle en cours de construction (4 flottants, recopiés dans +0x668) |
| +0x69c | nombre de jeux de coordonnées de texture du lot en cours (changement → vidage) |
| +0x6a0, +0x6a1, +0x6a2 | états déjà envoyés (mélange, test alpha, mode de combineur) : évitent de réécrire |
| +0x88 / +0x448 | blocs d'uniformes flottants préparés (en-tête 0x2C1) |

Vidage 0x192788 : en-tête 0x2C1 + `memcpy4` des uniformes (rectangles, puis texcoords si +0x26),
0x2B1 (uniforme entier : nombre de tours de boucle), 0x227 (tampon d'indices, 6 × rectangles, généré par 0x4944a8),
puis le bloc de dessin (0x40 octets, 0x49449c).

Le curseur d'écriture des commandes est le mot global **0x5f1dc0** (adresse du prochain mot).

## `Picture::DrawSelf` 0x48f398 (r0 = le panneau, r2 = le dessinateur)

1. Matériau : `0x191c4c(drawer, pane+0x138)`, puis textures `0x1921d8(drawer, material, 1)`.
2. Coordonnées de texture : calculées une fois par `0x48e060(drawer, material, pane+0x150, pane+0xd8)` → nombre en +0xd4, données en +0xd8 ; recalculées si +0xd2 = 0 ou si le matériau a le bit 2 de +0x4d.
3. Matrice : `0x11d370` recopie **pane+0x80** (matrice globale 3×4) dans le lot.
4. Position : `0x48dfd4(pane)` → (s0, s1), décalage selon le point d'ancrage ; taille = **pane+0x48, +0x4c** (largeur, hauteur).
5. Couleurs : `0x1924b8(drawer, pane+0x13c, alpha = pane+0xb5)` — les 4 couleurs de sommet.
6. Rectangle ajouté au lot ; vidage si le lot est plein.

## Matériau (pane+0x138)

| Champ | Rôle |
|---|---|
| +0x2c | bits 0x200 (test alpha à part : 0x48f894), 0x400 (mélange à part : 0x192394) |
| +0x30 | bits 0-1 : nombre de textures (0-3) ; bits 0x1c0 : combineur détaillé (0x192c50) |
| +0x34 | tableau des textures, 32 octets chacune (voir ci-dessous) |
| +0x4d | bit 1 : rien à régler ; bit 2 : coordonnées de texture à recalculer |
| +0x14 | couleur (mode sans texture) |

Sans réglage particulier : mélange par défaut (modèle 0x58e4cc, registres 0x100/0x101), test alpha éteint (0x104 = 0x61… en fait la valeur 0x61 sur 0x0F0104).
Combineurs : un modèle par nombre de textures (0 : 0x191ec0, 1 : 0x191f88, 2 : 0x1920b0, 3 : 0x192a6c), ou détaillé (0x192c50) ; +0x6a2 retient lequel est en place.

### Une texture (material+0x34 + 32 × n)

| Champ | Registre PICA |
|---|---|
| +0x04 | adresse physique (÷ 8 → 0x085) |
| +0x14 | format (0x08E) |
| +0x18 | taille, hauteur \| largeur << 16 (0x082) |
| +0x1c | filtre et répétition (0x083) |

0x1921d8 recopie ces champs dans le modèle 0x5f4990 (32 octets par unité) et allume les unités (0x080 = 0x11000 \| bits, 0x06F).

## Ce qu'il reste à décoder

- 0x48e060 (coordonnées de texture), 0x48dfd4 (ancrage), 0x1924b8 (couleurs) : courts.
- Les combineurs des 4 modèles et 0x192c50 (TEV détaillé).
- Le shader de 2D lui-même (RomFS) : quelle constante reçoit quoi, pour l'écrire en WGSL.
- Window (0x48e6a4), TextBox (0x48f724) et `nw::font`.
