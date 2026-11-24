# Rendu natif : supprimer la dernière émulation

Aujourd'hui, le jeu fabrique des listes de commandes PICA200 et picanative les
relit à chaque image pour dessiner avec le GPU du PC : c'est la seule partie
encore émulée (voir `PLAN.md`, « Principe : aucune émulation », point 3).
Ce document dit comment on la supprime, dans quel ordre, et comment on sait
qu'une étape est faite.

## Ce que le traceur a mesuré (NSMB2, 2 400 images)

`CTRN_TRACE_CMDS` (et `CTRN_TRACE_WATCH` pour les listes préparées) :

- **77 %** des mots envoyés au GPU sont des **listes préparées une fois,
  rejouées à chaque image** : la fonction du SDK `nngx` 0x541710 les recopie
  dans la liste de l'image ; elles n'ont reçu que 78 000 mots en 40 s ;
- **13 %** sont écrits à chaque image (matrices, paramètres qui bougent) ;
- **8 %** ne sont pas encore attribués (premier usage d'un tampon).

Les fonctions qui remplissent les listes préparées sont nommées dans
`NSMB2-RENDU.md` (analyse de la session « bibliothèques graphiques »).

## Le principe

Une fonction de bibliothèque qui fabrique des commandes est **remplacée** par
une fonction native qui fait le même travail avec l'API GPU du PC (Dawn/WebGPU,
comme picanative) — jamais par un interpréteur. Deux formes :

1. **Traduire une fois, au chargement.** Ce qui est figé (une liste préparée,
   un bloc de commandes d'un fichier de modèle) est converti une seule fois en
   objets natifs (pipelines, tampons, textures), puis rejoué en natif à chaque
   image. Une liste qui change (le jeu la réécrit) est retraduite à ce
   moment-là, pas à chaque image.
2. **Appeler directement.** Ce qui change à chaque image (une matrice, un
   matériau activé, un sprite 2D) passe par un appel natif avec ses
   paramètres, sans jamais devenir une commande.

## Le mécanisme

- **3dsrecomp reconnaît les fonctions par leurs octets**, comme ses « natives »
  reconnaissent les divisions de la bibliothèque C (`src/natives.rs`) : une
  signature par génération du SDK ou de la bibliothèque (2011, 2012, 2013+ pour
  `nn::gx` ; versions de NintendoWare), aucune adresse propre à un jeu dans le
  dépôt.
- Pour chacune, il écrit un **override** (`docs/overrides.md` de 3dsrecomp) qui
  passe la main à l'hôte par un appel natif (un numéro de service réservé),
  avec les arguments de la fonction.
- **L'hôte** (`src/host`, avec le moteur de rendu) fait le travail en natif.
  Si un cas n'est pas encore couvert, l'override appelle l'original
  (`RECOMP_ORIGINAL`) et le traceur le compte : rien ne casse, et l'on voit ce
  qui reste.

## L'ordre

1. **Le socle `nn::gx`** (les 10 jeux) : `nngx*` remplacées en natif, dont la
   **recopie des listes préparées**, qui devient « rejouer la traduction faite
   une fois ». Rien qu'avec ça, les 77 % de NSMB2 ne sont plus relus à chaque
   image.
2. **NintendoWare `nw::gfx`, version 2012** (NSMB2, Mario Kart 7) : le dessin
   des maillages et l'activation des matériaux en appels natifs ; les blocs de
   commandes des fichiers CGFX traduits au chargement.
3. **`nw::lyt` + `nw::font`** (menus et textes de 7 jeux).
4. Puis, dans l'ordre de `BIBLIOTHEQUES-GRAPHIQUES.md` : SM3DL, Grezzo (Zelda),
   `gfl2::gfx` (Soleil, Ultra-Lune), `nw::gfx` + `h3d` (X/Y, ROSA), Smash.

## Comment on sait qu'une étape est faite

- **Le traceur** : pour un jeu et un parcours donnés, la part des commandes
  encore fabriquées par la bibliothèque remplacée est **zéro**. Le but final :
  zéro mot envoyé au GPU pour tout le jeu, et picanative n'est plus chargé.
- **Les images** : chaque remplacement est comparé, capture par capture, au
  rendu par picanative (qui sert de référence tant qu'il existe), au même
  moment du jeu.
- **La vitesse** : le GPU travaille moins (il ne relit plus rien), mesuré par
  le journal de l'hôte.
