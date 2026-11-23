# Plan : zéro émulation, puis tous les jeux jouables (2026-11-23)

Ce plan détaille ce qu'il reste à faire après le travail du 2026-11-23 sur NSMB2. Il complète `PLAN.md` (phases A à H), `RENDU-NATIF.md` (le principe) et `BIBLIOTHEQUES-GRAPHIQUES.md` (qui utilise quoi).

## 0. Où on en est

**Ce qui est déjà natif, sans émulation :**
- **Le code des jeux** est traduit à l'avance en C par 3dsrecomp, puis compilé. Il n'y a aucun interpréteur de CPU.
- **La mémoire** est native (fastmem) : l'adresse du jeu est l'adresse de l'hôte.
- **Les programmes de shader** sont traduits à l'avance en WGSL (`pn_shaders`).
- **Le noyau et les services** sont réécrits en C++ (fichiers, manettes, son, ldr:ro…).
- **Les fonctions graphiques remplacées** (28 natives dans `tools/ctrn.py` / `src/host/gfx_native.cpp`) : l'hôte pose l'état du GPU directement et ne laisse dans la liste du jeu qu'un repère de 2 mots (registre 0x2F0), plus un bourrage sauté (0x2F1).

**La seule émulation qui reste : picanative relit encore des mots de commande PICA200.**
- Sur NSMB2, 1 200 images : 9,5 → 2,1 millions de mots.
- Sur une partie de 3 600 images : environ 13 millions de mots, dont 39 % de repères. Il reste environ 2 200 vrais mots par image.
- Sur les 9 autres jeux, rien n'est encore remplacé, sauf `nngxAdd3DCommand` et les copies de la bibliothèque C, qui sont communes.

**Outils de mesure en place :**

| Outil | Ce qu'il donne |
|---|---|
| `PN_REG_STATS=1` | mots relus par registre, sauts, listes qui écrivent 0x200 |
| `CTRN_TRACE_CMDS=fichier.csv` (+ `CTRN_TRACE_WATCH=adresse:taille`) | quelle fonction du jeu a écrit chaque mot, avec ses appelants |
| `[gfx] …` en fin de partie | appels de chaque native, part faite en natif, copies laissées au jeu, opérations vivantes |
| `CTRN_NO_NATIVES=n,m` | rend des natives au code du jeu, pour trouver celle qui dessine mal |

**Critère de fin, pour un jeu :** `PN_REG_STATS` ne montre plus que 0x2F0 et 0x2F1 sur tout un parcours du jeu. Ensuite, on retire la lecture de listes (§7).

---

## 1. Finir la 3D de NSMB2

Spec : `NSMB2-GFX.md`. Écrivains restants, mesurés dans la liste de la passe 3D (`CTRN_TRACE_WATCH=169fd000:20000,16a93000:20000`), par poids :

| # | Fonction | Ce qu'elle écrit | Approche |
|---|---|---|---|
| 1.1 | 0x18b7c0 (activateur de matériau) | matrices de texture (c-registres) | lire les SRT du matériau, poser les constantes (comme la caméra) |
| 1.2 | 0x18ae38 | environnement de lumière : 0x1C0 ambiante, 0x1C2 nombre de lumières, 0x1D9 permutation | décoder la fonction (non faite), puis poser les registres |
| 1.3 | 0x1719e0 / 0x171470 | c0-c5 du modèle | même méthode que les matrices déjà faites ; vérifier pourquoi l'aide n'est pas prise (en-tête différent ?) |
| 1.4 | 0x1b28e4 | préparation de la file (configuration des tampons de sommets 0x200+) | décoder ; probablement un bloc construit puis copié autrement |
| 1.5 | 0x177600 / 0x17c398, partie calculée | 0x1C3/0x1C4/0x149+ (éclairage), 0x08F/0x1C6/0x0E0, décalage de profondeur, couleurs constantes du TEV, masque 0x080 | les blocs préparés sont déjà natifs (copies) ; porter les mots calculés, sous-ressource par sous-ressource (§3.4 de la spec) |
| 1.6 | 0x170d58, 0x17155c, 0x1764b8 | lumières de fragment | à décoder (session d'analyse) |
| 1.7 | effets 0x3f3eb0, 0x39f2b0, 0x46baac, 0x471284 | particules (via des copies dont la destination n'est pas le curseur) | trouver leur curseur ou leur tampon ; spec à demander |
| 1.8 | 0x18005c | préparation paresseuse d'une primitive (une fois) | rien à faire si elle n'écrit pas de commandes ; à vérifier |

**Vérification :**
- captures à l'œil sur l'intro, un niveau, la carte du monde et un niveau sous l'eau, avec des effets ;
- `PN_REG_STATS` après chaque native ;
- `CTRN_NO_NATIVES` en cas de doute.

**Taille :** 2 à 3 sessions de travail, plus une demande d'analyse pour 1.2, 1.6 et 1.7.

## 2. Finir le socle `nn::gx` (commun aux 10 jeux)

| # | Fonction (NSMB2) | Rôle | Approche |
|---|---|---|---|
| 2.1 | 0x540ea4 | découpe de la liste : 0x25E, 0x111, 0x110, 0x010, puis une requête | native « après » : laisser la gestion des requêtes, remplacer les 4 commandes par une opération « vider le tampon d'image » (connaître la position avant et après) |
| 2.2 | 0x5409fc → 0x4b3d3c | appel d'une sous-liste (0x238-0x23C) : la passe 3D enregistrée | si la sous-liste ne contient que des repères : une opération qui les appelle, comme pour les listes enregistrées des menus |
| 2.3 | `nngxUseSavedCmdlist` (0x541710) | recopie des listes enregistrées | déjà réduite à une opération par copie ; garder la gestion d'états (masque) du SDK |
| 2.4 | `nngxRunCmdlist` / `nngxFlush3DCommand` / `nngxSwapBuffers` | envoi au GPU par la file GSP | à la fin (§7) : quand une liste ne contient que des repères, exécuter directement la suite d'opérations, sans GSP ni relecture |
| 2.5 | transferts (DisplayTransfer, TextureCopy, MemoryFill) | GX par GSP | déjà faits sur le GPU par picanative ; les exposer en appels natifs directs |

**Signatures :** 2012 (NSMB2, MK7), 2013+ (X/Y, ORAS, MM3D), 2011 (SM3DL, OoT). Une signature par génération du SDK, aucune adresse propre à un jeu.

## 3. Finir la 2D (`nw::lyt` + `nw::font`)

- Les matériaux non pris :
  - mélange personnalisé (0x192394) ;
  - TEV personnalisé (0x192c50) ;
  - 3 textures (0x192a6c).

  Ils représentent environ 2 % des réglages ; à décoder puis porter.
- `Window` : les aides de coins (0x191850…0x191b4c), à vérifier en jeu (cadres des menus de pause).
- Le chemin du texte « transformé » (`DispStringBuffer` +9 bit 1 : copie par 0x143138 avec matrice, listes chaînées 0x684-0x68c).
- **Généraliser** : `nw::lyt` existe en au moins 2 versions dans 7 jeux. Il faut des signatures par version et vérifier que les décalages des structures (dessinateur, matériau) sont les mêmes. Pour chaque version : relire le vidage, le matériau et les textures, et noter les décalages dans un tableau par version plutôt que dans le code.

## 4. Fiabilité : comparer sans dépendre du rythme du jeu

**Le problème :** une partie ne se rejoue pas à l'identique (le jeu suit l'horloge). Les captures divergent après quelques centaines d'images, et je vérifie alors à l'œil.

**À faire :**
1. **Mode « pas fixe » :** horloge du jeu avancée d'exactement 1/60 s par image, entrées rejouées image par image (`CTRN_INPUT_SCRIPT` existe), threads ordonnés de façon déterministe.
2. **A/B automatique :** la même partie avec et sans une native (`CTRN_NO_NATIVES`), captures comparées pixel par pixel, rapport des écarts.
3. **Un parcours de référence par jeu**, rejouable en une commande (Phase A de `PLAN.md`).
4. **Vérification de picanative :** pour chaque opération native, contrôle optionnel. On exécute aussi le code du jeu dans une copie, on compare les états PICA (`fingerprint`), et on signale la première différence.

**Taille :** une session pour 1-2 ; c'est ce qui rend tout le reste sûr.

## 5. Mario Kart 7 (le plus proche de NSMB2)

- Même SDK `nngx` (9 fonctions entières identiques), `nw::gfx` et `nw::lyt` de la même génération, 19 à 23 % de code commun avec NSMB2.
- **Étape 1 :** le faire tourner, ce qui n'a jamais été tenté : traduction, démarrage, course.
- **Étape 2 :** voir quelles natives de NSMB2 sont reconnues telles quelles (les signatures sont sans adresse), et ajuster celles qui diffèrent.
- **Étape 3 :** ce qui est propre à MK7 : son moteur (vue, ciseau, équivalents de 0x488da4), l'écran partagé, la carte du bas.
- La stéréoscopie et la 3D du haut à 60 i/s sont à mesurer.

## 6. Les autres jeux, bibliothèque par bibliothèque

Ordre : du plus réutilisable au plus isolé. Pour chaque jeu, d'abord jouable (bugs connus corrigés), puis traceur, spec par la session d'analyse, natives, vérification.

| Ordre | Jeux | Bibliothèques | À faire | Taille |
|---|---|---|---|---|
| 6.1 | Pokémon X/Y, Rubis Oméga, Saphir Alpha | `nw::lyt` (autre version), `gfl::grp::g3d` sur `h3d` | X/Y et ORAS partagent 80 % du code : une seule passe pour les 3. Scénario d'essai (marcher, combat, menus) ; ORAS : vérifier l'écrêtage du son (pics à 0 dBFS). | grande |
| 6.2 | Pokémon Soleil, Ultra-Lune | `gfl2::gfx` (GFGL, façon Direct3D) | API de haut niveau : remplacer au niveau GFGL (création de ressources, états, `Draw`) plutôt que des aides ; 90 % de code commun entre les deux. Jamais testés après les correctifs. | grande |
| 6.3 | Zelda OoT 3D, Majora's Mask 3D | Grezzo `gzw` (2 versions éloignées) | **MM3D d'abord : bug bloquant** après l'écran-titre (requête 0x6A4528 trouvée occupée → pointeur nul ; ordonnancement suspect). OoT tourne à 30 i/s. MM3D : transfert tuilé 2×2 fait sur le CPU (lent). | grande |
| 6.4 | Smash 3DS | `lib` de Sora (`Renderer`, `Gsb*`, `Lumen`) + `h3d` | Jamais testé après les correctifs. Moteur unique : tout est propre au jeu. | très grande |
| 6.5 | Super Mario 3D Land | `nw::gfx` ancienne (SDK 2011) | Déjà jouable (projet sm3dl-decomp) ; natives à adapter à l'ancienne version. | moyenne |
| 6.6 | Animal Crossing | ? | ROM chiffrée : il faut les clés pour la déchiffrer (fournies par l'utilisateur à partir de sa console). | — |

**Ce qui se réutilise d'un jeu à l'autre :**
- le mécanisme (repères, bourrage, opérations, durée de vie) ;
- les copies de la bibliothèque C ;
- `nngxAdd3DCommand` (même code dans 4 jeux) ;
- les outils de mesure.

**Ce qui ne se réutilise pas :** les moteurs propres à chaque jeu (vue, ciseau, effets).

## 7. La dernière marche : plus aucune liste relue

Quand un jeu ne laisse plus que des repères :
1. **Les listes deviennent des suites d'opérations :** au lieu de relire les mots (même seulement 0x2F0/0x2F1), l'hôte garde pour chaque liste la suite des opérations qu'il y a marquées, dans l'ordre. `nngxRunCmdlist` exécute cette suite directement.
2. **picanative ne garde que le rendu :** `CommandProcessor::run` n'est plus appelé pour ce jeu. On garde l'état (registres comme description de l'état GPU) le temps de passer à l'étape 3.
3. **Une API d'état native :** les opérations décrivent l'état avec des structures natives (`draw::Config`, uniformes, textures, cibles), plus avec des numéros de registres PICA. C'est le passage de « poser des registres » à « appeler un moteur de rendu ». À faire bibliothèque par bibliothèque, en commençant par `nw::lyt` (le plus simple : un rectangle, un matériau).
4. **Retirer GSP comme file** : les transferts et remplissages deviennent des appels directs.

Critère : `PN_REG_STATS` = 0 mot sur un parcours complet, et `pn_command_list` jamais appelé.

## 8. Ce qui n'est pas graphique

| Domaine | État | À faire |
|---|---|---|
| Son (DSP) | Le microcode DSP n'est pas exécuté : l'hôte fait le mixage, et l'AAC passe par Media Foundation | vérifier le mixage contre la console (écrêtage ORAS) ; Android : AAC par MediaCodec |
| Services | réécrits (fs, hid, apt, ldr:ro, y2r, ir…) | compléter selon les jeux (cam, mic, friends, ac, boss : réponses neutres documentées) |
| Ordonnanceur | fidèle à la console (sans « soulagement anti-famine ») | MM3D : prouver ou écarter la piste de l'ordonnancement |
| Mémoire | fastmem | **fuite** observée (1,4 → 2,3 Go) : mesurer à nouveau maintenant que les opérations sont libérées, puis chercher ailleurs (surfaces, textures de picanative) |
| Polices système | remplaçante si `shared_font.bin` est absent | documenter l'extraction depuis la console de l'utilisateur |

## 9. Ensuite : mieux que la console (Phase H de `PLAN.md`)

Quand le rendu passe par des opérations natives, c'est plus simple :
- **Performance :**
  - picanative relit moins → CPU ;
  - cache de pipelines déjà à l'avance ;
  - mesurer le temps GPU par image (Intel intégré : 15-32 %).
- **Fluidité :**
  - 60 i/s stables : cadence par vsync, pas de saccade au premier pipeline ;
  - les jeux à 30 i/s (OoT, MM3D) restent à leur rythme, sauf mod.
- **Qualité :**
  - échelle de résolution (déjà ×1 à ×8) ;
  - filtrage des textures ;
  - anticrénelage (plus facile une fois l'état natif) ;
  - écran large au cas par cas.
- **Mods :** format Luma3DS (`luma/titles/<id>/romfs/`, `code.ips`), Phase C de `PLAN.md`.
- **Android** (Thor, deux écrans) : picanative a déjà une version Android ; il manque le NDK sur la machine.
- **Distribution :** lanceur avec liste des jeux, cache, mises à jour (Phase G).

## 10. Ordre proposé et repères de taille

| Ordre | Étape | Taille (sessions de travail) |
|---|---|---|
| 1 | §1 + §2.1-2.2 : NSMB2 sans un seul mot hors repères | 3-4 |
| 2 | §4.1-4.2 : pas fixe et A/B automatique | 1-2 |
| 3 | §3 : fin de la 2D, puis version suivante de `nw::lyt` | 2 |
| 4 | §5 : Mario Kart 7 jouable puis natif | 3-5 |
| 5 | §6.3 : MM3D débloqué, OoT/MM natifs | 5-8 |
| 6 | §6.1 : X/Y, ORAS | 6-10 |
| 7 | §6.2 : Soleil, Ultra-Lune | 5-8 |
| 8 | §6.4 : Smash | 8-12 |
| 9 | §6.5 : SM3DL | 2-3 |
| 10 | §7 : plus de lecture de listes, API d'état native | 4-6 |
| 11 | §8 et §9 : son, fuite mémoire, perf, qualité, mods, Android | en continu |

« Session de travail » = une longue séance comme celles du 2026-11-23. Ces tailles sont des ordres de grandeur, pas des promesses. Les moteurs propres à un jeu (Smash, Grezzo, Game Freak) sont les plus incertains.

**Travail en parallèle possible :**
- la session d'analyse écrit les specs du jeu suivant pendant que les natives du jeu en cours avancent ;
- une session « fiabilité » (§4) peut avancer seule ;
- règle : jamais deux sessions sur les mêmes fichiers (`gfx_native.cpp` est le point chaud ; à découper en un fichier par bibliothèque avant de paralléliser).

## 11. Dettes connues (à ne pas oublier)

- `gfx_native.cpp` grossit : le découper en un fichier par bibliothèque (nngx, lyt/font, gfx, moteurs de jeux) avec une table des natives.
- Les natives de NSMB2 propres au jeu (vue, ciseau) sont dans le même fichier que les génériques : les séparer, avec un dossier par jeu.
- Les signatures de 7 mots (copies de 24 octets, aides de vecteurs) peuvent trouver des copies identiques ailleurs dans un jeu. Elles ne s'appliquent que si la destination est le curseur, mais il faut le vérifier par jeu.
- La durée de vie des opérations suppose qu'une liste d'image est consommée en 8 images.
- Après une recompilation de picanative seul, il faut copier `picanative_engine.dll` à la main dans `ctrnative/build`.
- Commits locaux seulement (ctrnative, picanative, 3dsrecomp sur `multi-jeux`) : rien n'est poussé.
