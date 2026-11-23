# Plan

Le but : faire tourner les jeux 3DS nativement, avec leurs mods, de façon
fiable. Le moteur vient du port natif de Super Mario 3D Land (SM3DL), qui reste
le jeu de référence : rien ne passe à l'étape suivante tant qu'il n'est pas
vérifié sur lui, par des essais qui se rejouent seuls et disent oui ou non.

## Où on en est vraiment

Fait (étapes 1 à 3) : le moteur seul, les services par requêtes IPC (aucun
remplacement de fonction propre à un jeu), le lanceur depuis une ROM
décryptée (`tools/ctrn.py`), qui trouve aussi les constructeurs C++.

Vérifié sur SM3DL, sans fenêtre, par des captures d'écran :

- depuis la seule ROM et un cache vide, le jeu arrive au choix du fichier ;
- avec une sauvegarde existante et l'appui automatique sur A, il va de
  l'introduction à la carte du monde 1 ;
- le code décompressé et les 616 constructeurs sont identiques à ceux de
  l'ancien port (sous Zakuro).

**Pas vérifié du tout :**

| Point | Pourquoi ce n'est pas vérifié |
|---|---|
| Jouer un niveau | aucun niveau lancé, ni terminé, dans ctrnative |
| Le son | jamais écouté ni enregistré |
| La vitesse réelle | jamais mesurée en fenêtre ; sans fenêtre, 30 i/s avec captures |
| La fenêtre, le clavier, la manette | jamais lancés dans ctrnative |
| La sauvegarde | formatage vu, mais ni fichier créé, ni partie relue après relance |
| La durée | aucune partie de plus de 2 minutes |
| L'image | jamais comparée à la référence, seulement regardée |
| Android | rien |

## Phase A — Des essais qui se rejouent et se jugent seuls

Aujourd'hui l'hôte avance au rythme de l'horloge réelle : deux parties avec le
même script de touches ne vont pas au même endroit (selon la charge de la
machine, les captures, la fenêtre). Rien n'est donc rejouable.

- **A1. Mode déterministe** (`CTRN_LOCKSTEP=1`). L'horloge du jeu et les
  retours d'écran avancent avec le jeu, pas avec l'horloge réelle : une image
  de l'hôte = une image finie par le jeu, horloge système dérivée du nombre
  d'images. Critère : deux parties du même script donnent les mêmes captures,
  octet pour octet.
- **A2. Le son enregistré** (`CTRN_AUDIO_WAV=fichier`). Ce que le DSP produit,
  en WAV, et une analyse : durée de silence, coupures, niveau. Critère : un
  enregistrement de la musique de l'écran titre sans coupure.
- **A3. Les mesures** en fenêtre : temps de chaque image (jeu, rendu,
  présentation), images en retard. Rapport en fin de partie.
- **A4. La comparaison d'images** avec la référence de l'ancien port (la trace
  `boot-1-1` rendue par gfxref, gardée en local car elle vient du jeu) :
  écart moyen et pire écart par capture, au même moment du jeu (A1).
- **A5. Un essai = un fichier** (`tests/<jeu>/<scénario>.toml` : script de
  touches, sauvegarde de départ, captures attendues, critères), et
  `tools/check.py` qui les rejoue et dit lesquels passent. Les sauvegardes et
  références restent en local ; seuls les scénarios vont dans le dépôt.

## Phase B — La campagne SM3DL

Chaque essai a un critère chiffré et passe par `tools/check.py`.

1. **Démarrage** : sauvegarde neuve, choix du fichier A, introduction, carte.
   Critère : `GameData.bin` créé ; à la relance, le fichier A est relu (la
   carte, pas l'introduction).
2. **Le parcours 1-1, 1-2, 1-3** au script de touches (la demande de départ).
   Critère : l'écran de fin de chaque niveau atteint, capture comparée.
3. **Le son** : la musique du titre et de 1-1 enregistrée (A2), sans coupure
   de plus de 10 ms, au bon niveau.
4. **La vitesse en fenêtre** : 1-1 à 60 i/s, au moins 99 % des images à
   l'heure, à l'échelle 1 et 2 ; temps par image relevé.
5. **L'endurance** : 60 minutes de jeu au script, sans arrêt, mémoire stable.
6. **Les entrées réelles** : le clavier par la fenêtre (touches envoyées par
   Windows, vérifiable sans joueur) ; la manette XInput à essayer à la main
   (liste de 5 vérifications pour qui joue).
7. **Les anciennes sauvegardes** (rangées par Zakuro) relues telles quelles.
8. **L'image** : les captures de la trace `boot-1-1` à moins de 1 % de pixels
   écartés (A4).

## Phase C — Les mods, au format de Luma3DS

- **C1. LayeredFS** : `luma/titles/<id>/romfs/` remplace des fichiers de la
  RomFS. Le jeu lit la RomFS comme une image (niveau 3) : le moteur reconstruit
  cette image avec les fichiers remplacés (en cache, à la première partie).
  Essai : un mod de texture ou de texte connu, capture comparée.
- **C2. Code** : `code.ips` ou `code.bin` changent le code, donc le module
  est retraduit (une fois par combinaison de mods, cache) ; `exheader.bin`
  aussi. Essai : un patch IPS simple (une valeur du jeu changée), effet vu.
- **C3. Le choix des mods** au lancement (`--mods` et un dossier par défaut),
  et leur liste dans le journal.
- Plus tard : les plugins `.3gx` (code injecté pendant la partie : il faut un
  interpréteur ARM de secours).

## Phase D — Un deuxième jeu : Mario Kart 7

Ce qui manquera, à mesure que le jeu le demande : d'autres services, d'autres
commandes, peut-être des modules CRO (code chargé à la volée par `ldr:ro`,
que 3dsrecomp traduit mais que l'hôte ne charge pas encore). Même campagne
courte que B (démarrage, une course au script, son, vitesse, sauvegarde).

## Phase E — La couverture du système

À faire quand un jeu en a besoin, chacun avec son essai :

- applets : clavier logiciel, sélecteur de Mii, messages d'erreur ;
- `ldr:ro` (modules CRO), `ptm` (batterie, podomètre), `ac`/`frd`/`news`
  (réseau absent, réponses neutres), caméras et micro (images et son vides) ;
- New 3DS : la mémoire étendue (exheader 0x20E), `ir:rst` (ZL, ZR,
  C-stick), la vitesse doublée ;
- la langue et la région de la console (aujourd'hui fixées) ;
- la 3D stéréo (l'œil droit, aujourd'hui le même que le gauche).

## Phase F — Android

L'application générique : la ROM choisie au lancement, le module compilé sur
PC pour Android (le lanceur sait déjà compiler, il faut la cible ARM64 et
l'empaquetage). Essai : la campagne B courte sur l'AYN Thor.

## Phase G — La distribution

Des versions publiées (l'hôte compilé, le lanceur), avec ce qu'il faut pour
traduire un jeu chez soi (3dsrecomp, clang) ou une façon de s'en passer.

## Principe : aucune émulation

Décision du 2026-11-23 : rien ne doit être émulé. Le code du jeu l'est déjà
(traduit à l'avance, exécuté en natif) ; le système (fichiers, son, threads)
est une couche de compatibilité, comme Wine. Restent à supprimer :

1. ~~**La mémoire de la 3DS** derrière une table de pages consultée à chaque
   accès~~ : fait (fastmem, ABI 6 de 3dsrecomp). La mémoire physique est une
   section partagée, chaque mapping du jeu une vue de celle-ci dans 4 Gio
   réservés, en lecture seule là où le jeu ne peut pas écrire ; un accès =
   `base + adresse`. Un accès hors de ce qui est mappé arrête la partie en
   nommant l'adresse (« the game wrote to … ») au lieu d'être ignoré.
2. **Les shaders du GPU** traduits pendant la partie : traduits à l'avance,
   avec le reste du jeu.
3. **Les listes de commandes PICA200** que le jeu fabrique et que picanative
   relit : la bibliothèque graphique du SDK que chaque jeu embarque remplacée
   par une version native, bibliothèque par bibliothèque. picanative ne sert
   plus qu'aux jeux dont la bibliothèque n'est pas encore faite, puis à aucun.

## Phase H — Mieux que la console, pour tous les jeux

Une fois les jeux fonctionnels, des améliorations du moteur, donc valables pour
chaque jeu sans travail propre à l'un d'eux :

- **les performances du logiciel** : le temps par image (GPU, code du jeu,
  transferts) mesuré par jeu (A3), puis réduit là où il se perd ; la première
  traduction plus courte (niveau d'optimisation, indices fournis par jeu) ;
- **la fluidité** : 60 images/s pour les jeux qui tournent à 30 sur la console,
  quand c'est possible sans changer leur logique ;
- **la qualité d'image** : la résolution interne (déjà là, `CTRN_SCALE`), le
  filtrage des textures, l'anticrénelage, l'écran large ;
- **les mods** : la gestion de ceux de la phase C (choix, ordre, activation par
  jeu) ;
- et ce qui s'y ajoutera.

Chaque amélioration se vérifie sur les jeux de la campagne, comparée à
l'image sans elle.

## Ordre

A, puis B en entier, avant C et D : tant que SM3DL n'est pas vérifié jusqu'au
bout, un défaut trouvé plus tard dans un autre jeu ne se distinguera pas d'un
défaut du moteur.

## Hors champ

Les ROM chiffrées (à décrypter avant), le jeu en ligne, les échanges
StreetPass. La décompilation de SM3DL reste possible par-dessus (un « mod » de
code), mais n'est plus nécessaire pour qu'un jeu tourne.
