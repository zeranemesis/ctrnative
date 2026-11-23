# Plan

Le moteur vient du port natif de Super Mario 3D Land (sm3dl-decomp), où il
s'appuyait sur des remplacements de fonctions à des adresses propres à ce jeu.
Les étapes le rendent valable pour tous les jeux.

1. **Le moteur seul** — fait. Noyau, mémoire, services, GPU (gsp), son (dsp),
   fenêtre picanative ; titre et identifiant du jeu lus dans son exheader ;
   variables `CTRN_*`.
2. **Les services par requêtes IPC.** Le port de SM3DL passe encore par
   77 remplacements de fonctions du SDK à des adresses fixes (fichiers, manette,
   écran tactile, GPU, son). Chacun devient la réponse du service au niveau IPC
   (`fs:USER`, `hid:USER`, `gsp::Gpu`, `dsp::DSP`, `apt:U`…), commune à tous
   les jeux. Contrôle : SM3DL tourne avec un module généré sans ses overrides.
3. **Le lanceur.** Une ROM décryptée en entrée : extraction de l'ExeFS, de
   l'exheader et de la RomFS, 3dsrecomp, compilation du module, cache par
   identifiant du jeu et empreinte du code, lancement.
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
