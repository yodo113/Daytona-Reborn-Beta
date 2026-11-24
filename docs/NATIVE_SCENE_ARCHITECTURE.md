# Daytona Arcade Native — architecture de la scène moderne

## 1. Base choisie

| Projet | Licence | État | Décision |
|---|---|---|---|
| **alphanu1/daytona-arcade-recomp** | BSD-3-Clause | 242 ★, actif, ROM locale acceptée, i960/TGP recompilés, audio/FFB/contrôles, rendu D3D12/Vulkan/Metal | **Base retenue** |
| sp00nznet/model2recomp | MIT | runtime Model 2 générique, 14 ★ | Référence de rendu, pas de jeu complet |
| dmanlfc/sm2-emu | GPL-3.0 | émulateur Model 1/2 | Référence de comparaison |
| mamedev/mame | GPL-2.0+ | oracle de référence | Oracle uniquement |

Le build de référence a été mesuré avec la ROM locale : **1794 présentations en 15 s**, médiane **8,33 ms**, soit **~120 FPS de présentation**. La simulation arcade reste à sa cadence d'origine (57,52 Hz) ; le rendu est déjà découplé au niveau présentation.

## 2. Frontière technique constatée

- `src/runtime/geo.cpp` est le géométrizer Model 2 natif. `Geo::model2_3d_process_polygon` reçoit déjà des sommets **en espace écran/clip** ; la transformation objet est appliquée plus tôt par `M2_GEO_POINT`/`M2_GEO_VECTOR` avec `geo->matrix`.
- `src/app/gpu/gpu_renderer.cpp` reconstruit chaque frame un vertex buffer de polygones et échantillonne la texture RAM, le luma et la translation couleur. Ce n'est pas une scène persistante.
- Conséquence : pour une scène moderne résidente, il faut capturer **avant** la projection, au niveau des commandes de points/matrices du display list, et conserver l'identité objet/matériau.

## 3. Architecture cible

```
simulation i960/TGP (57,52 Hz, inchangée)
        │ snapshot immuable par frame
        ▼
extracteur géométrie monde (hook geo.cpp, avant projection)
        │ triangles + UV + matériau + matrice objet
        ▼
construction de scène (objets persistants, meshes GPU, textures)
        │ scene graph statique/dynamique
        ▼
renderer D3D12 (Windows) / Vulkan (Linux) via SDL_GPU ou backend natif
        │ HDR FP16 scRGB, tonemap, PQ/HDR10
        ▼
présentation 120+ Hz (interpolation, sans toucher au tick)
```

### Règles

- La simulation, l'audio, le FFB et les contrôles restent ceux de la base.
- Le renderer historique reste disponible comme repli et oracle pixel.
- Aucune donnée de jeu, ROM, capture ou binaire généré dans Git.
- Le renderer moderne doit pouvoir être désactivé sans changer le comportement.

## 4. Premier jalon

1. Hook de capture **monde** dans `geo.cpp` (points avant `M2_GEO_POINT`, matrice courante, matériau/texheader), dump binaire d'une frame de course.
2. Viewer D3D12 autonome : buffers persistants, texture array, soleil directionnel, ombre simple ; vérifier que la scène correspond à la frame d'origine.
3. Intégration optionnelle dans le jeu, HUD/tilemaps historiques conservés ; mesure CPU/GPU/présentation.
4. Ensuite : PBR, ombres temps réel, RT, HDR10, interpolation 120.

## 5. Risques

- Identité objet absente du flux actuel : à reconstruire par matrice/display list.
- Textures Model 2 indexées/palettisées : conversion à valider.
- Le renderer historique est l'oracle pixel ; toute divergence doit être mesurée.
- HDR10+ Gaming / Dolby Vision : métadonnées et transmission à qualifier séparément ; ne pas confondre rendu élargi et format natif.

## 2 bis. Correction après revue technique

La revue technique a corrigé les hypothèses suivantes :

- `geo->matrix` est une matrice **modèle-vue**, pas monde ; il n'existe pas de commande caméra/monde séparée dans le flux. La capture « avant `M2_GEO_POINT` » doit donc être appelée **espace objet/modèle-vue**, pas monde.
- La clé de source géométrique est `(pool, oba, obc)` ; elle n'identifie pas une instance. Le texheader n'identifie pas à lui seul le matériau et avance par `tho` ; `luma` est calculé à part.
- Un scene graph interpolé ne peut pas servir d'oracle pixel : `Geo::polys` est après cull/clip/troncature, avec une profondeur synthétique et sans correspondance de sommets entre ticks.
- Architecture retenue : second lecteur à `geo_object_data`, cache de meshes objet clés `(pool, oba, obc, mode)`, ancre relative `W_i = inverse(M_ancre) * M_i`, deux passes oracle/native, HUD/tilemaps historiques.
- Test de réfutation : 32 frames, comptage des sommets avant cull par source, clique rigide des matrices relatives ; seuils 50 %.

## Occurrences distinctes dans une image (10 novembre)

`(pool, oba, obc, mode)` décrit une source de géométrie, pas une instance.
Deux appels `geo_object_data` peuvent partager cette clé et même leur matrice.
`GeoPoly::object_command` conserve maintenant le numéro d'émission de l'appel,
à partir de 1 et réinitialisé à chaque frame. Le numéro est attribué avant
le parsing et le culling, y compris pour un appel sans polygone visible.

Le cache regroupe les primitives par commande et conserve l'ordre de première
émission. Il partage ensuite les variantes de maillage entre ces occurrences.
Une provenance nulle est traitée prudemment comme une primitive indépendante.
La RAM rapide est exclue même si le bit ROM est aussi présent dans l'adresse.
Le format expérimental MSC2 v1 reste inchangé : l'ordre de ses instances est
déterministe, mais il n'exporte pas encore les numéros de commande.

Cette correction suit la séparation géométrie/rendu de la section « Geometry
(TGP) and rendering » du document de conception. Elle ne transforme pas une
matrice modèle-vue en matrice monde. Les numéros de commande ne constituent
pas une identité temporelle et ne doivent pas servir seuls à interpoler.
Le cache reste tributaire des sous-ensembles visibles, fige encore certains
paramètres de primitive et utilise une signature sans comparaison complète
en cas de collision. La capture complète avant culling, les matériaux dynamiques
et l'appariement temporel restent des travaux distincts.

## Provenance : constat après les contre-exemples temporels

Les tests PR10 montrent qu'une correspondance mutuellement unique dans une
borne de déplacement peut transférer l'identité d'un objet disparu à son
remplaçant. Même une longue chaîne de correspondances ne prouve donc pas
l'identité. Une matrice modèle-vue et une clé de maillage ne suffisent pas.

Une précision consiste à ajouter les paramètres texture et la position de commande
aux traces. Ce sont des diagnostics, jamais des identifiants persistants.
Attention à un décalage dans cette proposition : `Geo::parse` consomme l'opcode
avant `geo_object_data`, qui consomme ensuite quatre mots. À ce stade,
`input.i - 4` désigne le premier argument, pas l'opcode (`input.i - 5`). Une
instrumentation doit plutôt conserver l'index avant lecture dans `parse`,
après résolution des sauts, et distinguer clairement offsets en mots et octets.
Cette observation vient du code, pas d'une nouvelle capture runtime.

Une recherche en amont doit aussi distinguer les producteurs :
`TgpBoard::mem_w` (TGP), `buffer_w` (écritures masquées côté i960) et `push_geo`
(port géométrie) écrivent tous la RAM de commandes. Une sonde sur le seul port
géométrie ne couvre pas toutes les écritures. `M2Board` possède un contexte CPU,
mais la précision de son IP au moment des écritures et la signification des
registres comme pointeurs d'instances doivent encore être démontrées.

Prochain lot de capture : positions de commande/arguments, origine d'écriture,
masque et génération de frame, avec test de sauts et réutilisation de buffer.
Conserver les captures brutes localement et comparer simulation/counters avec
et sans sonde. Ne pas activer l'interpolation à partir de ces métadonnées seules.

## Observation des ticks terminés

`m2run IMAGES_DIR 1700 --pc-solo 2 --snapshot-stamps` ajoute au journal
standard un en-tête `snapshot_stamp,tick,time_ns,discontinuity_before`,
une ligne préfixée `snapshot_stamp,` par tick terminé et
`snapshot_stamp_end,1700` après une exécution réussie. Conserver le
journal dans un répertoire de travail privé. Pour lire les données,
écarter l'en-tête et les messages habituels de m2run, puis vérifier le
marqueur de fin, le nombre de lignes et le code de sortie.

Le tick commence à zéro et le premier porte le marqueur de rupture. Le
temps écoulé en nanosecondes est observé après le retour de `run_frame`
sur l'horloge monotone de ce processus. Les lignes existent aussi quand
le jeu saute un dessin. Ces valeurs décrivent la fin du travail de
simulation observé dans cette exécution.

Pour une utilisation côté présentation, le producteur doit choisir une
horloge commune et attribuer un identifiant distinct à chaque GameLoop
dont des snapshots coexistent. Le contrôle de l'identifiant empêche de
mélanger deux sessions si le premier tick de la nouvelle session manque.

Le mode `--scene-stamps` écrit
`scene_stamp,tick,time_ns,discontinuity_before,last_drawn_tick`.
Chaque ligne correspond à un tick terminé. Le dernier champ contient le
tick de la dernière scène terminée sur cette carte. Il reste stable quand
un dessin est sauté et il est vide si aucune scène n'a encore été produite.
Avec les deux options, les champs de timing proviennent du même relevé.
`scene_stamp_end,N` donne le nombre total de ticks d'une exécution réussie.
Cette trace mesure la production de la scène côté simulation. Elle ne
compte pas les images présentées à l'écran.

La sonde `m2run IMAGES_DIR 1550 --scene-pair 1499` conserve les données
MSC2 des dessins 1499 et 1500, avec leur contexte de dessin copié, puis
vérifie les deux snapshots après la fin de la simulation. Elle imprime
`scene_pair_verified,1499,1500,N_A,N_B` sur succès. Les deux ticks
doivent être dessinés, donc utiliser cette sonde sans saut de dessin.
L'encodage en mémoire est demandé uniquement pour ces deux ticks et
réutilise l'encodeur du fichier MSC2 historique. Les données restent
dans le processus. La sonde couvre la géométrie et le contexte CPU.

## Contrôle GPU d'une pose intermédiaire

Avec les shaders DXIL du lecteur MSC2 et un dossier de sortie neuf,
lancer `python tests/check_scene_intermediate_gpu.py EXE VS.dxil PS.dxil SORTIE`.
Le contrôle utilise un triangle synthétique à trois positions connues.
Il vérifie les appels de dessin, le masque exact, la couleur et trois
empreintes d'image distinctes. Une position intermédiaire volontairement
décalée doit être rejetée. Le jeu n'intervient pas dans ce contrôle.

Pour relier ce test au calcul de timing, compiler en Release la cible
`test_snapshot_timing` et ajouter le chemin du binaire construit en dernier
argument :

```powershell
python tests/check_scene_intermediate_gpu.py EXE VS.dxil PS.dxil SORTIE TIMING_TEST_EXE
```

Le binaire échantillonne le timing C++ à 1000, 1500 et 2000 ns et interpole
les positions X 1 et 5. Le test exige les trois positions 1, 3 et 5. Il
rend également deux cas échantillonnés à 1500 ns, avec rupture explicite
ou identifiants de session différents. Le binaire vérifie la garde dans
les deux cas. Le rendu confirme que la position renvoyée donne exactement
l'image finale. Cette preuve couvre une fixture hors écran sans ROM.

Pour vérifier la réutilisation du périphérique et de la pipeline D3D12,
générer d'abord une référence PPM en mode mono-scène pour chacune des
deux scènes. Chaque scène fournit son propre contexte 496 × 384.

```powershell
m2sceneshot A.msc2 VS.dxil PS.dxil A.ppm A.context
m2sceneshot B.msc2 VS.dxil PS.dxil B.ppm B.context
m2sceneshot --verify-pair A.msc2 A.context B.msc2 B.context VS.dxil PS.dxil A.ppm B.ppm
```

Le dernier appel rend A, B puis A hors écran sur un seul périphérique.
Il compare les deux premiers rendus aux PPM et vérifie que le dernier
reproduit le premier. Après chaque lecture GPU, il attend le fence et
libère les ressources propres au dessin. Ce mode utilise le même shader
sans textures pour les deux scènes et n'écrit aucun fichier. Le test
synthétique couvre aussi A, scène vide, A.

Pour mesurer les transferts réels de sommets sur cette même paire :

```powershell
m2sceneshot --arena-aba A.msc2 A.context B.msc2 B.context VS.dxil PS.dxil A.ppm B.ppm
```

Le prototype conserve les plages de sommets de A et B dans un tampon GPU
de capacité fixe. Il vérifie les trois images, puis affiche les octets
soumis aux copies SDL pour chaque passe. Le troisième passage doit
transférer zéro octet de sommets. Les objets gardent leurs propres
transformations et rectangles de découpe même s'ils partagent une plage.
La comparaison d'octets tranche les collisions de hachage.

Ce test attend la fin du travail GPU à chaque passe. Il ne gère ni
l'éviction ni les textures, et ne mesure ni la durée des transferts
ni la fréquence des images affichées en jeu.

Le test synthétique ci-dessous crée ses propres scènes. Deux instances
partagent les sommets dans A ; B dessine une nouvelle plage en premier.
Il vérifie la stabilité de la silhouette de gauche, la modification de
celle de droite, les 60/60/0 octets transférés et le rejet de références
inversées. Utiliser un dossier de sortie neuf.

```powershell
python tests/check_msc2_vertex_arena.py m2sceneshot.exe m2sceneshot.exe VS.dxil PS.dxil NEW_OUTPUT_DIR
```

Le mode `--two-in-flight-aba` soumet A et B au même périphérique avant
d'attendre leurs deux fences ensemble. Il compare les lectures aux
références, rejoue B puis A et vérifie que les sommets restent résidents.
Les deux soumissions utilisent des cibles couleur et profondeur et des
tampons de lecture distincts. Le test exige six allocations de cibles
sur les deux paires, y compris après une annulation confirmée de B.
Une paire ne peut réutiliser ces ressources qu'après la fin des deux
lectures, ou après une annulation confirmée et l'attente des commandes
déjà soumises. La mesure porte sur l'ordre des appels CPU, les allocations
et les octets envoyés aux copies SDL.

```powershell
m2sceneshot --two-in-flight-aba A.msc2 A.context B.msc2 B.context VS.dxil PS.dxil A.ppm B.ppm
python tests/check_msc2_two_submission.py m2sceneshot.exe m2sceneshot.exe VS.dxil PS.dxil NEW_OUTPUT_DIR
```

Le test synthétique vérifie également l'annulation de B avant sa
soumission et trois erreurs injectées. Après une annulation confirmée,
A reste disponible. Après une annulation incertaine ou une fence
manquante, le test exige un appel d'attente du périphérique et le refus
de réutiliser l'arène. Ces essais simulent les erreurs sur un périphérique
D3D12 fonctionnel.
