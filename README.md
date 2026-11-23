# ctrnative

Fait tourner un jeu Nintendo 3DS nativement sur PC (et bientôt Android), sans
émulateur : le code ARM du jeu est traduit à l'avance en C par
[3dsrecomp](https://github.com/fearkov/3dsrecomp), compilé, puis exécuté sur
le noyau et les services de la console réécrits ici. Le GPU PICA200 est rendu
par [picanative](https://github.com/zeranemesis/picanative) (WebGPU).

Ce dépôt ne contient rien d'un jeu : chacun fournit sa propre ROM décryptée,
dont il a le droit de faire une copie.

**État : un premier jeu tourne sans rien de propre à lui** (Super Mario 3D
Land : de l'introduction à la carte du monde 1, avec un module traduit sans
aucun remplacement de fonction). Les étapes suivantes : le lanceur, les mods,
un deuxième jeu, voir [docs/PLAN.md](docs/PLAN.md).

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

L'hôte se lance aussi seul : `build/ctrnative <module> jeu.3ds` (il lit le
code, décompressé, l'exheader, le titre et la RomFS dans la ROM).

| Variable | Effet |
|---|---|
| `CTRN_USER_DIR` | le dossier des sauvegardes |
| `CTRN_HEADLESS=1`, `CTRN_FRAMES=n` | sans fenêtre, arrêt après n images |
| `CTRN_SCALE` | résolution interne (2 par défaut) |
| `CTRN_LAYOUT` | `stacked`, `side`, `large` ou `top` (F10 change) |
| `CTRN_SHOT_EVERY=n`, `CTRN_SHOT_DIR` | les deux écrans toutes les n images (`f*.ppm`, `b*.ppm`) |
| `CTRN_PRESS_A=1`, `CTRN_TAP_AT=x,y` | essais sans joueur : A, ou un toucher, 6 images sur 60 |
| `CTRN_INPUT_SCRIPT` | essais : lignes `début fin TOUCHES… [x=..] [y=..]`, en images |
| `CTRN_LOG_THREADS=1` | chaque thread créé, avec sa priorité et sa pile |
| `CTRN_UNCACHED=1` | le moteur relit chaque surface (lent, pour comparer) |

## Remerciements

[3dsrecomp](https://github.com/fearkov/3dsrecomp) et
[Zakuro](https://github.com/fearkov/zakuro) (fearkov, MIT) : le format du code
traduit, et les règles d'ordonnancement que ce noyau suit.

## Licence

MIT.
