# Plan

Le moteur vient du port natif de Super Mario 3D Land (sm3dl-decomp), où il
s'appuyait sur des remplacements de fonctions à des adresses propres à ce jeu.
Les étapes le rendent valable pour tous les jeux.

1. **Le moteur seul** — fait. Noyau, mémoire, services, GPU (gsp), son (dsp),
   fenêtre picanative ; titre et identifiant du jeu lus dans son exheader ;
   variables `CTRN_*`.
2. **Les services par requêtes IPC** — fait. Le port de SM3DL passait par
   77 remplacements de fonctions du SDK à des adresses fixes. Le moteur répond
   maintenant au niveau IPC, comme pour tous les jeux : la RomFS ouverte
   directement (archive 3), la sauvegarde (archive 4 : formatage, infos,
   ouverture, dossiers lus pour de vrai), la manette et l'écran tactile écrits
   à chaque image dans la mémoire partagée de HID. Contrôle : SM3DL, module
   généré sans aucun remplacement, va de l'introduction à la carte du monde 1.
3. **Le lanceur** — fait. `tools/ctrn.py jeu.3ds` : 3dsrecomp, compilation
   du module, cache par identifiant du jeu et empreinte de son code, puis
   lancement. L'hôte lit lui-même la ROM : le code (décompressé), l'exheader,
   le titre (icône SMDH) et la RomFS.
4. **Les mods, au format de Luma3DS.**
   - `luma/titles/<id>/romfs/` : les fichiers remplacent ceux de la RomFS
     (LayeredFS), sans reconstruire.
   - `code.ips` ou `code.bin` : le code change, donc le module est retraduit,
     une fois par combinaison de mods (cache).
   - plugins `.3gx` : plus tard (code injecté pendant la partie, il faut un
     interpréteur).
5. **Un deuxième jeu** (Mario Kart 7) : ce qui manque au moteur.
6. **Android** (AYN Thor) : l'application générique, la ROM choisie au
   lancement.
