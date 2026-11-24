# NSMB2 : qui écrit les commandes GPU (2026-11-23)

Analyse statique du code de NSMB2 (ROM de `C:\jeu 3ds\extrait`, code chargé à 0x100000). Je n'ai rien lancé.
Les adresses viennent du traceur de commandes (2 400 images) : 77 % des mots passent par 0x541710, 13 % par 0x114dec.

Comment j'ai nommé les fonctions :
- **noms exportés** : le module principal de Soleil (CRS) exporte des noms. Je compare les instructions en masquant les décalages des appels et des chargements de constantes.
- **RTTI de X/Y** : la table virtuelle des classes nommées. Je compare les suites d'instructions et l'ordre des appels.
- **API du SDK** : je compare le nombre et le rôle des paramètres avec l'API `nngx` documentée.
- **comportement** : les registres PICA cités dans les constantes et la forme des données copiées.

NSMB2 n'a ni RTTI ni nom de fichier source dans son code. Les symboles de SM3DL (décompilation, 12 924 noms `al`/`sead`/`nn`) ne recoupent aucune de ces fonctions : le moteur de NSMB2 est `sead` avec son propre code de jeu.

Légende : **V** = vérifié (même code qu'une fonction nommée, ou mêmes paramètres que l'API documentée et même code dans 3 autres jeux). **S+** = supposé, avec de bons indices. **S** = supposé d'après le comportement seulement.

## En bref

- **Les 77 % viennent des layouts 2D (`nw::lyt`), dessinés une seule fois puis rejoués.** Un objet du moteur (code de jeu, tables 0x5c03c4/0x5d2244) possède sa propre liste `nngx`.
  - Quand le layout change (drapeau +0xed), 0x47d5f8 l'enregistre : `nngxStartCmdlistSave`, dessin `nw::lyt`, puis `nngxStopCmdlistSave`.
  - À chaque image, 0x47d258 le recopie dans la liste courante par `nngxUseSavedCmdlist(…, copycmd = vrai)`. C'est le `memcpy` à 0x119cf4.
- **Les 13 % sont des copies de petits modèles de commandes par `__aeabi_memcpy4`** (0x114dec). Elles viennent de `nw::lyt` (matériau, texture, quadrilatères), de `nw::font` (uniformes, lots de caractères) et du moteur (fenêtre d'affichage, ciseaux).
- **Ce qu'il faut remplacer en premier, c'est la paire Save/UseSaved, avant `nw::lyt`.** Il suffit de traduire une fois le bloc enregistré, puis de rejouer la traduction tant que 0x47d5f8 ne réenregistre pas. Cela couvre les 77 % sans toucher `nw::lyt` (voir la fin).
- Le 3D (`nw::gfx`) n'apparaît pas dans cette liste : il est forcément dans les 10 % restants (non vérifié).

## 1. SDK `nngx` (couche commune)

| Adresse | Nom | Sûreté | Ce qu'elle fait |
|---|---|---|---|
| 0x541710 | `nngxUseSavedCmdlist(cmdlist, bufferoffset, buffersize, requestid, requestsize, statemask, copycmd)` | **V** | Retrouve la liste par son nom (table de hachage `id & 0x1f`). Marque comme sales les états de `statemask` (13 bits → drapeaux d'état). Recopie la zone `[offset, offset+size)` de la liste enregistrée dans le tampon courant avec `memcpy` (0x119cf4). Recopie ensuite les requêtes (28 octets chacune) avec `memcpy4` en recalant leurs adresses. Si `copycmd` est faux, elle ne recopie pas et ajoute un saut. Même code dans X/Y 0x131144, ORAS 0x131414 et MK7 0x58be3c (96 mots identiques). |
| 0x5407e4 | `nngxAdd3DCommand(bufferaddr, buffersize, copycmd)` | **V** | Si `copycmd` est vrai, `memcpy4` dans le tampon courant. Sinon, elle ajoute une requête « sous-liste » (adresse, taille). Elle a 88 appelants. Même code dans X/Y 0x16d680, ORAS 0x16dfe8 et MK7 0x58ad8c. |
| 0x541204 | `nngxStartCmdlistSave()` | **S+** | Aligne le tampon sur 16 octets (commande de bourrage) et lève le drapeau « sauvegarde » (+0x13). Appelée par 0x47d5f8 juste avant le dessin. |
| 0x5412e8 | `nngxStopCmdlistSave(&bufferoffset, &buffersize, &requestid, &requestsize)` | **S+** | Ses 4 pointeurs remplissent exactement les champs +0xd4…+0xe0 que relit `UseSavedCmdlist` dans 0x47d258. |
| 0x5407cc | aide → `nngxAdd3DCommand(blk.addr, blk.size, blk.copy != 0)` | **V** (code) | Prend une structure `{adresse, taille, copie}` et enchaîne sur 0x5407e4. Je ne sais pas si elle vient du SDK ou de `nw`. |
| 0x10a740 / 0x540c6c / 0x10a990 / 0x10a8a8 | `nngxBindCmdlist` / `nngxClearCmdlist` / `nngxGenCmdlists` / `nngxCmdlistStorage` | **V** | Retrouvées par les noms exportés de Soleil. |
| 0x119cf4 | `__aeabi_memcpy` | **V** | Nom exporté de Soleil, 34 mots identiques. Appelée seulement par `UseSavedCmdlist` dans la chaîne mesurée. |
| 0x114dec | `__aeabi_memcpy4` | **V** | Nom exporté de Soleil, 29 mots identiques. Elle a 143 appelants : c'est « l'écrivain direct » du traceur. Elle écrit ce qu'on lui donne et n'appartient à aucune bibliothèque graphique. |

## 2. Moteur (sead + code de NSMB2) : le cache de layouts

Les chaînes du jeu nomment les couches de dessin `sead` : `sead::TopRootDraw`, `TopSysDraw`, `TopAppDraw`, `TopAppDrawFinal`, et la même chose pour `Btm`. Les fonctions ci-dessous n'ont pas de nom. Leur rôle se lit dans le code.

| Adresse | Nom proposé | Sûreté | Ce qu'elle fait |
|---|---|---|---|
| 0x47d51c | `LayoutCache::create` (nom inventé) | S | Fait `nngxGenCmdlists`, `nngxBindCmdlist` et `nngxCmdlistStorage` : l'objet a **sa propre liste de commandes** (nom à +0xd0). Elle a 73 appelants. |
| 0x47d5f4 → 0x47d5f8 | `LayoutCache::record` | **S+** | Le premier appel fait `vfunc[5]` (initialisation). Ensuite, seulement si +0xed (sale) : sauve la liste liée, `Bind(+0xd0)`, `nngxClearCmdlist`, `nngxStartCmdlistSave`, `drawer->vfunc[3](1)` (DrawBegin), `0x1930d8(drawer, layout +0x10, drawInfo +0x30)`, `drawer->vfunc[4](1)` (DrawEnd), `nngxStopCmdlistSave(+0xd4, +0xd8, +0xdc, +0xe0)`, puis relie l'ancienne liste. `drawer` = singleton 0x47de8c + 0x360. |
| 0x47d258 | `LayoutCache::draw` (rejoue) | **S+** | Bloc de 16 octets (4 mots), copié : 3 mots d'un modèle global (+0x130) et `(h<<16) \| (w-1)`, calculés depuis la taille à +0x28. C'est sans doute le ciseau ou la fenêtre d'affichage. Il est envoyé par `nngxAdd3DCommand(…, 16, 1)`, en version recadrée si +0x104. Puis : décalage stéréo (±+0xf4 selon l'œil, +0x100 = 0x400/0x410), `drawer.SetParallax` (0x1a98c8), `DrawBegin(drawInfo)` (0x1aa924), `nngxUseSavedCmdlist(+0xd0…+0xe0, statemask +0xe4, copycmd +0xec)`, et `drawer->vfunc[4](0)`. |
| 0x47df8c | réglage de la fenêtre d'affichage | S | Lit des matrices et dimensions globales (+0x148, +0x168), convertit des float32 en **float24 PICA** et écrit le bloc par `memcpy4`. Appelée par 0x47c4b0, 0x47dcc4 et 0x10e834. |
| 0x38e5cc / 0x38e6d0 | portes « si actif, dessiner » | **V** (code) | Fonctions feuilles : `if (this+0xa07) goto 0x47dcc4` (ou `0x47c4b0`). Elles sont rangées dans une table de pointeurs du moteur (0x5c041c / 0x5c0414). |
| 0x137b84 | méthode de dessin d'une scène du jeu | S | Rangée dans la table 0x598104. Elle passe par le singleton 0x47de8c, appelle 0x38e67c et écrit les uniformes de shader (en-tête 0x2C0, 4 mots). |
| 0x47de8c | accès au singleton de rendu | S | +0x360 = l'objet `nw::lyt::Drawer`. |

Note : les tables qui contiennent ces fonctions alternent pointeur et zéro (0x5c03c4, 0x5d2244). Elles ressemblent à des pointeurs de méthode ARM `{fonction, ajustement}`, comme ceux qu'utilisent les délégués `sead`. Ce n'est pas vérifié.

## 3. `nw::lyt` (layouts 2D) : ce qui est enregistré dans le cache

Pour ces fonctions, X/Y donne un code de référence nommé, d'une version plus récente. Aucune n'est identique, mais l'ordre des appels concorde.

| Adresse | Nom proposé | Sûreté | Indices |
|---|---|---|---|
| 0x48e6a4 | `nw::lyt::Window::DrawSelf` | **S+** | Même ordre d'appels que l'entrée 29 de `nw::lyt::Window` dans X/Y (0x48c514) : le contenu, puis 4 coins, et pour chaque morceau 4 appels (matériau, texture, coordonnées du coin, quadrilatère). Sa table 0x59c3ec est parallèle à celles de Picture et TextBox. Le nom « DrawSelf » de l'entrée 29 est lui-même supposé. |
| 0x48f398 | `nw::lyt::Picture::DrawSelf` | **S+** | Ressemble à l'entrée 29 de `Picture` dans X/Y (0x48d3b0). Appels : matériau, texture, `memcpy4`, quadrilatère. |
| 0x48f724 | `nw::lyt::TextBox::DrawSelf` | **S+** | Très proche (0,40 à 0,48) de l'entrée 29 de `TextBox` dans X/Y (0x48d920). Appelle 0x1920dc pour le texte. |
| 0x191c4c | `nw::lyt::Material::SetupGraphics` (ou équivalent) | S+ | Pendant de X/Y 0x30e40c d'après l'ordre des appels. Écrit les couleurs constantes TEV (0x0DB, 0x0FB) et le test alpha (0x104). Recopie le modèle de mélange 0x58e4cc (registres 0x100/0x101). Elle a 15 appelants (tous les DrawSelf). |
| 0x1921d8 | réglage des textures du matériau | S+ | Pendant de X/Y 0x30e998. Écrit 0x080 (TEXUNIT_CONFIG) et 0x06F. Recopie le modèle 0x5f4990 (registres 0x82, 0x85…) avec `memcpy4`. Elle a 17 appelants. |
| 0x192934 | couleurs constantes TEV (partie du matériau) | S | Appelée par 0x191c4c. Écrit 0x0DB. |
| 0x193050 | copie du bloc d'un quadrilatère | S | Pendant de X/Y 0x30f700. Un `memcpy4` d'un bloc préparé, 17 appelants. |
| 0x1930d8 | `Drawer::Draw(layout, drawInfo)` / `Layout::Draw` | **S+** | Fonction feuille : si le layout a une racine dont le drapeau +0xb7 bit 0 (visible) est levé, elle enchaîne sur 0x48df4c (dessin de l'arbre des panneaux). C'est elle qu'enregistre 0x47d5f8. |
| 0x1920dc | dessin du texte d'un TextBox | S | Appelle 0x1a98dc, 0x1a9578, 0x1a93a8 (police) et 0x1921d8 (texture). |

## 4. `nw::font` / `Drawer` (texte et état du dessinateur)

| Adresse | Nom proposé | Sûreté | Ce qu'elle fait |
|---|---|---|---|
| 0x1a98c8 | `Drawer::SetParallax(parallax, dLevel, addDist)` | **S+** | Range 3 flottants (et une constante) à +0x74. Appelée par 0x47d258 avec le décalage de l'œil. |
| 0x1aa924 | `Drawer::DrawBegin(drawInfo)` | S | Fait `vfunc[3](0)`, puis enchaîne sur 0x1aa954 avec `&drawInfo+4` (les matrices). |
| 0x1aaef4 | `Drawer::DrawEnd(flag)` | S | Si un lot est en attente (+0x24), le vide (0x192788). Puis, si `!(flag & 1)`, rejoue le bloc de restauration d'état +0x14 par 0x5407cc → `nngxAdd3DCommand`. |
| 0x192788 | vidage d'un lot de rectangles (glyphes ou quadrilatères) | S | Écrit 0x227 (tampon d'indices), 0x2B1 (uniforme entier, nombre de tours) et 0x2C1 (uniformes flottants). Trois `memcpy4`. Elle a 19 appelants. |
| 0x1a98dc | vidage des uniformes en attente | **S+** | Construit l'en-tête 0x2C1 (nombre = 4 × +0x25), puis `memcpy4` du bloc +0x238 dans le tampon courant. |
| 0x1aae20 | chargement d'une matrice dans le shader | **V** (code) | Fonction feuille. Écrit 0x2C0 = 0x80000023 (uniformes flottants à partir de c35), puis 16 flottants par 0x2C1. |
| 0x1a93a8 | construction d'un lot de caractères (`RectDrawer`) | S | Appelle 0x1aae20 (matrice) et 0x1aae8c, puis `memcpy4`. Contient la constante 0x80808081 (division par 255, donc des couleurs). |
| 0x1aa9ac | réglages du `RectDrawer` (début de dessin) | S | 1 352 octets, 5 `memcpy4`. Écrit 0x227, 0x2C0 et 0x2C1. |

## Ce que ça change pour ctrnative

1. **Remplacer d'abord `nngxStartCmdlistSave`, `nngxStopCmdlistSave` et `nngxUseSavedCmdlist`.**
   - Au `Stop`, traduire une fois la zone enregistrée et la garder, avec comme clé (liste, offset, taille).
   - À chaque `UseSaved`, rejouer la traduction sans recopier ni relire les mots.
   - L'enregistrement ne se refait que quand 0x47d5f8 voit +0xed. La clé change alors d'elle-même : `Clear` puis `Save` donnent un nouvel offset.
   - Ces fonctions sont du SDK, communes à NSMB2, MK7, X/Y et ORAS (même code).
   - C'est le seul point de coupe qui supprime les 77 % sans réécrire `nw::lyt`.
2. **Les 13 % de `memcpy4` ne se coupent pas à `memcpy4`**, qui sert à tout. Il faut les prendre chez leurs appelants : le matériau, les quadrilatères et le texte de `nw::lyt`/`nw::font` ci-dessus.
3. Les modèles de commandes statiques (0x58e4cc, 0x5f4990, le modèle +0x130 du ciseau) sont des données fixes. On peut les traduire une fois au chargement.

## Limites

- Aucune exécution : les rôles « S » sont lus dans le code, pas observés.
- Les noms `nw::lyt` viennent d'une version plus récente (X/Y). L'entrée 29 n'est pas nommée par la RTTI : « DrawSelf » est déduit de ce que fait la fonction.
- Le nom de la classe du moteur (« LayoutCache ») est inventé.
- Je n'ai pas cherché qui écrit les 10 % restants (le 3D `nw::gfx` probablement).
- Outils (Python + capstone) : `fuzzy.py`, `pair.py`, `calls.py`, `lits.py`, `match.py` et `vtmap.py`. Ils sont dans le dossier de travail de cette session, et je les copie dans `bibliotheques-outils/` du dossier de notes partagé.
