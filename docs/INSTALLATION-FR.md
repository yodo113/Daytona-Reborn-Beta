# Daytona Arcade Native : installer et jouer (Windows)

Daytona Arcade Native est une édition PC de *Daytona USA* (Sega Model 2,
Revision A, 1994). Elle s'appuie sur la recompilation statique
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp)
d'alphanu1, et y ajoute un menu PC, l'écran large, une distance d'affichage
allongée et un décor en volume.

**Aucune donnée du jeu n'est fournie.** Le jeu est reconstruit sur votre
ordinateur, à partir de **votre propre copie** de la ROM. C'est pour cette
raison qu'il n'y a pas d'exécutable à télécharger.

## Ce qu'il vous faut

1. **Windows 10 ou 11, 64 bits**, avec une connexion internet pour la
   première installation et une dizaine de Go libres.
2. **Une carte graphique compatible Direct3D 12**, recommandée. Le jeu
   fonctionne aussi sans, mais les améliorations visuelles demandent
   Direct3D 12.
3. **Votre ROM *Daytona USA* Revision A**, au format MAME `daytona.zip` ou
   `.7z`. Elle doit contenir `epr-16722a.12`, `epr-16723a.13`,
   `epr-16724a.6` et `epr-16725a.7`. Les autres versions de Daytona USA
   sont refusées, même renommées.
4. **Git**. Dans PowerShell :

       winget install Git.Git

   Fermez ensuite PowerShell et rouvrez-le.

## 1. Télécharger le projet

Dans PowerShell, placez-vous dans le dossier de votre choix, puis tapez :

    git clone https://github.com/yodo113/Daytona-Reborn-Beta.git
    cd Daytona-Reborn-Beta

Toutes les commandes suivantes se tapent depuis ce dossier.

## 2. Mettre votre ROM en place

Créez le dossier `roms` et copiez-y votre ROM, nommée **exactement**
`daytona.zip` (ou `daytona.7z`) :

    mkdir roms
    copy C:\chemin\vers\votre\daytona.zip roms\daytona.zip

Le dossier `roms` reste sur votre ordinateur : il n'est jamais envoyé nulle
part.

## 3. Construire le jeu

    powershell -ExecutionPolicy Bypass -File setup.ps1

La première fois, l'installation télécharge les outils nécessaires (CMake,
Python, Visual Studio Build Tools avec Clang). Windows peut vous demander
une autorisation pour les installer. Elle recompile ensuite le jeu à
partir de votre ROM et le construit. Comptez de quelques minutes à plus
d'une demi-heure, selon votre machine et votre connexion.

Si un message indique que Python « n'est pas encore dans le PATH », fermez
PowerShell, rouvrez-le, puis relancez la même commande.

C'est terminé quand s'affiche `== Done`, suivi de la commande à lancer pour
jouer.

## 4. Jouer

Lancez la commande indiquée à la fin de l'installation :

    build-daytona\Release\daytona.exe

Un menu s'ouvre :
choisissez le mode de jeu, la boîte de vitesses, puis **Jouer**.

### Les réglages (bouton « Réglages »)

Dans **Réglages**, le menu **Langue / Language** en haut de la fenêtre
change la langue de l'interface.

Dans la partie **Affichage**, **Format de l'image** : **Arcade 4:3** pour
l'image de la borne (centrée, avec des bandes sur un écran large),
**Automatique** pour la forme de la fenêtre, ou 16:10, 16:9, 21:9.

Pour profiter des améliorations, choisissez d'abord **Rendu : Matériel**
dans les réglages d'affichage. Le réglage par défaut, **Logiciel**, calcule
la 3D exactement comme la carte d'arcade ; les améliorations sont prévues
et vérifiées avec le rendu **Matériel** (Direct3D 12).

Dans la partie **Améliorations** :

- **Distance d'affichage** : plus loin, le décor apparaît plus tôt.
- **Décor en volume** : sapins en volume, tribunes pleines, piliers sous le
  pont de Dinosaur Canyon. Pris en compte au prochain lancement.
- **Voitures adverses toujours détaillées** : le modèle le plus détaillé à
  toutes les distances. Sur la borne, une voiture change de modèle selon sa
  distance et le nombre de voitures proches, et ce changement se voit.
  Pris en compte au prochain lancement.
- **Lissage 120 images/s (expérimental)** : images intermédiaires entre
  celles du jeu, pour un mouvement plus fluide sur un écran à 120 Hz ou
  plus. Demande le rendu Matériel et le mode de dessin « Double tampon ».
  Pris en compte au prochain lancement.
- **Couleurs de l'écran** : les couleurs telles qu'une borne à tube les
  montrait (rendu Matériel).

- Les nouveaux profils utilisent les couleurs **Borne** (gamma 2,4).
- Dans **Réglages**, **Affichage**, activez les **Ombres du soleil (ray
  tracing, expérimental)** uniquement si vous souhaitez les essayer. Il faut
  Direct3D 12 avec DXR 1.1 et un redémarrage. La lumière du ciel est facultative.
- Le menu **Audio** permet de choisir la musique. Les noms des fichiers
  nécessaires pour Saturn / Windows et Dreamcast figurent dans les
  [notes de version](NOTES-BETA-FR.md#fichiers-de-musique-locaux).

Le volant, la manette et le clavier se règlent dans la même fenêtre.

## En cas de problème

- Le guide détaillé (en anglais), avec les erreurs courantes :
  [getting-started.md](getting-started.md).
- Signalez un problème sur la page *Issues* du projet sur GitHub. Indiquez
  votre version de Windows, votre carte graphique, et la fin du texte
  affiché par `setup.ps1`.
