# Daytona Arcade Native : bêta 0.11

Version publique pour Windows. Installation :
[INSTALLATION-FR.md](INSTALLATION-FR.md). English notes:
[NOTES-BETA-EN.md](NOTES-BETA-EN.md).

## D'où vient le projet

Daytona Arcade Native s'appuie sur la recompilation statique
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp)
d'alphanu1. Le programme de la borne y est traduit en code C++ natif, sans
émulateur. Cette édition garde la simulation d'origine, donc la conduite et
la course de la borne, et travaille sur l'affichage et le confort de jeu sur
PC.

## Nouveautés de la 0.11

- **Sept langues** dans le lanceur. Français, anglais, portugais, espagnol,
  chinois, japonais et arabe.
- **Couleurs de la borne**. Le préréglage Borne (gamma 2,4) est celui des
  nouveaux profils. Les profils déjà enregistrés gardent leurs réglages.
- **Ray tracing expérimental**, désactivé par défaut. Sous Direct3D 12 avec
  DXR 1.1, il ajoute les ombres du soleil sur le décor et la voiture, la
  lumière du ciel en option et des rayons ancrés dans le décor. Le jeu
  continue avec le rendu standard si cette fonction est indisponible.
  Les changements prennent effet au prochain lancement.
- **Musique de course**. Choisissez Arcade, Saturn / Windows ou Dreamcast
  dans les réglages audio. Le mode Arcade utilise le son de votre ROM.
  Les autres modes lisent vos fichiers WAV locaux, tirés de votre propre
  copie du jeu. Les fichiers manquants laissent jouer la musique d’origine.
- **Décor en option**. Les appuis de Dinosaur Canyon sont accessibles via
  `M2_CANYON_FILL=1` et `M2_CANYON_ROCK_ABUTMENT=1`. En Expert, avec la
  distance d’affichage +2, `M2_EXPERT_TOWER=1` rend visible plus tôt le
  modèle de la tour du jeu sur son approche.
- **Fluidité**. Le lissage reste optionnel et le ray tracing traite les
  grandes images suréchantillonnées à la résolution de l’écran pour
  réduire son coût.

### Fichiers de musique locaux

Créez `musiques-locales` à côté de `daytona.exe`. Les fichiers attendus
sont des WAV PCM 16 bits, stéréo, 44,1 kHz.

| Circuit | Dans `musiques-locales/saturn/` ou `musiques-locales/windows/` | Dans `musiques-locales/dreamcast/` |
| --- | --- | --- |
| Beginner | `07-lets-go-away.wav` (ou `07.wav`) | `adxb_055.wav` |
| Advanced | `06-the-king-of-speed.wav` | `adxb_063.wav` |
| Expert | `08-sky-high.wav` | `adxb_060.wav` |

## Nouveautés de la 0.10

Ces nouveautés répondent aux demandes reçues sous les vidéos et sur GitHub.

- **Langue de l'interface** : ouvrez les Réglages du lanceur et utilisez
  « Langue / Language ». Français et anglais pour commencer. Le choix est gardé, et le
  premier lancement suit la langue de Windows. Le portugais, l'espagnol, le
  chinois, le japonais et l'arabe sont en préparation.
- **Format de l'image** : il est maintenant en tête des réglages
  d'affichage. Choix : **Arcade 4:3** (l'image de la borne, centrée avec des
  bandes), Automatique (la forme de la fenêtre), 16:10, 16:9 ou 21:9. Si la
  fenêtre est plus large que le format choisi, l'image est centrée avec des
  bandes latérales. Une fenêtre plus étroite limite la largeur de la vue
  en écran large.
- **Lissage 120 images/s réparé** : sur une course, 20 des 39 voitures
  adverses tremblaient ou se dédoublaient. Elles sont maintenant toutes
  suivies, et le mouvement est plus régulier. L'option reste à cocher.

## Déjà dans la 0.9

- Un menu PC : course normale ou Time Attack, boîte automatique ou
  manuelle, réglages.
- L'écran large : le décor et les voitures sont visibles sur les côtés.
- Une distance d'affichage réglable. Le pont d'Expert est visible dès le
  départ.
- Un décor en volume, en option : sapins, tribunes, piliers du pont de
  Dinosaur Canyon.
- Une courbe d'écran façon borne à tube, et un réglage de saturation.
- Les voitures adverses toujours détaillées, en option : un seul modèle à
  toutes les distances.

## Limites connues

- **Lissage 120 images/s** : il demande le rendu Matériel et le mode de
  dessin « Double tampon ». La cadence dépend de l'écran et de la carte
  graphique.
- **Voitures adverses** : ce sont les modèles d'origine de la borne. Une
  reconstruction plus détaillée est prévue.
- Seule la ROM *Daytona USA* Revision A (`daytona`) donne accès au menu PC
  et aux améliorations.

## Aucune donnée du jeu

Le dépôt ne contient ni ROM ni code tiré de la ROM. Le jeu est reconstruit
sur votre ordinateur à partir de votre propre copie.
