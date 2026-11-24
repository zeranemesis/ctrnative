# ctrnative

Fait tourner un jeu Nintendo 3DS nativement sur PC (et bientôt Android), sans
émulateur : le code ARM du jeu est traduit à l'avance en C par
[3dsrecomp](https://github.com/fearkov/3dsrecomp), compilé, puis exécuté sur
le noyau et les services de la console réécrits ici. Le GPU PICA200 est rendu
par [picanative](https://github.com/zeranemesis/picanative) (WebGPU).

Ce dépôt ne contient rien d'un jeu : chacun fournit sa propre ROM décryptée,
dont il a le droit de faire une copie.

**État : un premier jeu démarre sans rien de propre à lui** (Super Mario 3D
Land, lancé depuis sa seule ROM : de l'introduction à la carte du monde 1).
Rien n'est encore vérifié au-delà : ni un niveau joué, ni le son, ni la
vitesse en fenêtre. La suite, en commençant par des essais qui se rejouent
seuls : [docs/PLAN.md](docs/PLAN.md).

## Principes

- **Un seul moteur pour tous les jeux.** Le noyau (threads, mémoire,
  synchronisation) et les services répondent aux requêtes du système (IPC),
  les mêmes pour tous les jeux, et non à des adresses propres à un jeu.
- **Les mods de la 3DS tels quels.** Le moteur lit l'arborescence de Luma3DS
  (`luma/titles/<id du jeu>/romfs/`, `code.ips`) : un mod fait pour la
  console marche sans changement.
- **Natif, pas émulé.** Le code du jeu est du code machine compilé ; un jeu
  décompilé (fonctions réécrites en C++) se branche par-dessus, fonction par
  fonction.

## Construire

Il faut clang et clang-cl (LLVM), CMake, Ninja, Python 3,
[3dsrecomp](https://github.com/fearkov/3dsrecomp) compilé, et picanative
construit à côté (`../picanative`, voir son README).

```bash
# l'hôte ; recomp.h, le même pour tous les jeux, est dans abi/ de 3dsrecomp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRECOMP_DIR=<3dsrecomp>/abi
cmake --build build
```

## Lancer un jeu

```bash
python tools/ctrn.py jeu.3ds --3dsrecomp <3dsrecomp.exe>
```

La ROM doit être décryptée (.3ds/.cci, ou .cxi). Au premier lancement, le
lanceur traduit le code du jeu avec 3dsrecomp et le compile (quelques
minutes) ; le module est gardé en cache (`%LOCALAPPDATA%\ctrnative`), par
identifiant du jeu et empreinte de son code. Les sauvegardes vont dans
`<cache>/user`.

Les shaders du jeu sont traduits à ce moment-là aussi : le lanceur cherche les
binaires de shaders (blocs DVLB) dans le code et dans chaque fichier de la
RomFS, tel quel ou décompressé (LZ10/LZ11, Yaz0, LzS), les garde dans
`<cache>/games/<jeu>/shaders/`, et `pn_shaders` (picanative, copié à côté de
l'hôte) les traduit dans `shaders.pnvs`, que l'hôte charge au démarrage. En fin
de partie, l'hôte dit combien de programmes étaient traduits à l'avance et
combien l'ont été pendant la partie (chacun est aussi journalisé) ; le but est
zéro. La traduction est refaite quand picanative change.

L'hôte se lance aussi seul : `build/ctrnative <module> jeu.3ds` (il lit le
code, décompressé, l'exheader, le titre et la RomFS dans la ROM).

| Variable | Effet |
|---|---|
| `CTRN_USER_DIR` | le dossier des sauvegardes |
| `CTRN_HEADLESS=1`, `CTRN_FRAMES=n` | sans fenêtre, arrêt après n images |
| `CTRN_SCALE` | résolution interne, 1 à 4 (1 par défaut : les pixels de la console) |
| `CTRN_LAYOUT` | `stacked`, `side`, `large` ou `top` (F10 change) |
| `CTRN_SHOT_EVERY=n`, `CTRN_SHOT_DIR` | les deux écrans toutes les n images (`f*.ppm`, `b*.ppm`) |
| `CTRN_PRESS_A=1`, `CTRN_TAP_AT=x,y` | essais sans joueur : A, ou un toucher, 6 images sur 60 |
| `CTRN_INPUT_SCRIPT` | essais : lignes `début fin TOUCHES… [x=..] [y=..]`, en images |
| `CTRN_LOG_THREADS=1` | chaque thread créé, avec sa priorité et sa pile |
| `CTRN_LOG_IPC=1` | chaque requête aux services, au fil de la partie (sauf GPU, DSP, fichiers) |
| `CTRN_SVC_STATS=1` | en fin de partie : appels système, requêtes, et ce que chaque thread attend |
| `CTRN_STRICT=1` | pas de réponse par défaut des services périphériques : arrêt au premier manquant |
| `CTRN_UNCACHED=1` | le moteur relit chaque surface (lent, pour comparer) |

Toutes les 10 s, l'hôte affiche les images/s du jeu et la part du temps où son
code travaillait : près de 100 %, c'est le processeur du PC qui limite ; plus
bas, le jeu attend le rythme de la console (il n'y a rien à gagner côté CPU).

Les services périphériques pas encore écrits (réseau, amis, StreetPass,
contenus téléchargeables…) répondent « succès, rien à signaler », comme une
console hors ligne, et le journal le dit une fois par commande. Les applets
(clavier, sélecteur de Mii, messages d'erreur) se ferment aussitôt avec la
réponse d'un joueur qui valide : un nom « Player », un Mii standard.

Le lanceur trouve dans le code ce que 3dsrecomp ne voit pas : les
constructeurs C++, les tables de sauts et les appels écrits à la main en
assembleur (décodeurs vidéo). Il faut un 3dsrecomp où un indice donné par
`--hints` devient toujours une entrée, même au milieu d'une fonction déjà
trouvée ou à la place d'une fausse fonction Thumb (`apply_hints`, build.rs).

## Remerciements

[3dsrecomp](https://github.com/fearkov/3dsrecomp) et
[Zakuro](https://github.com/fearkov/zakuro) (fearkov, MIT) : le format du code
traduit, et les règles d'ordonnancement que ce noyau suit.

## Licence

MIT.
