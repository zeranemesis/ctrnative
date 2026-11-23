# ctrnative

Fait tourner un jeu Nintendo 3DS nativement sur PC (et bientôt Android), sans
émulateur : le code ARM du jeu est traduit à l'avance en C par
[3dsrecomp](https://github.com/fearkov/3dsrecomp), compilé, puis exécuté sur
le noyau et les services de la console réécrits ici. Le GPU PICA200 est rendu
par [picanative](https://github.com/zeranemesis/picanative) (WebGPU).

Ce dépôt ne contient rien d'un jeu : chacun fournit sa propre ROM décryptée,
dont il a le droit de faire une copie.

**État : un premier jeu tourne de bout en bout** (Super Mario 3D Land, à
60 images/s dans sa fenêtre). Le moteur vient de son port natif et devient
générique étape par étape, voir [docs/PLAN.md](docs/PLAN.md).

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

Il faut clang-cl (LLVM), CMake et Ninja, et picanative construit à côté
(`../picanative`, voir son README).

```bash
# le module du jeu : 3dsrecomp traduit son code, puis on le compile
3dsrecomp generate jeu.3ds build/recomp
# l'hôte
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRECOMP_DIR=build/recomp
cmake --build build
```

## Lancer

```bash
CTRN_ROM=jeu.3ds build/ctrnative.exe <module du jeu> <code.bin> <exheader.bin>
```

| Variable | Effet |
|---|---|
| `CTRN_ROM` | la ROM décryptée (.3ds) : sa RomFS et son ExeFS |
| `CTRN_USER_DIR` | le dossier des sauvegardes |
| `CTRN_HEADLESS=1`, `CTRN_FRAMES=n` | sans fenêtre, arrêt après n images |
| `CTRN_SCALE` | résolution interne (2 par défaut) |
| `CTRN_LAYOUT` | `stacked`, `side`, `large` ou `top` (F10 change) |
| `CTRN_SHOT_EVERY=n`, `CTRN_SHOT_DIR` | l'écran du haut toutes les n images |

## Remerciements

[3dsrecomp](https://github.com/fearkov/3dsrecomp) et
[Zakuro](https://github.com/fearkov/zakuro) (fearkov, MIT) : le format du code
traduit, et les règles d'ordonnancement que ce noyau suit.

## Licence

MIT.
