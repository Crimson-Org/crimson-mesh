<p align="center">
  <img src="./assets/icons/scalable/apps/io.github.CrimsonOrg.CrimsonMesh.svg" alt="Crimson Mesh logo" width="128" height="128">
</p>

<h1 align="center">Crimson Mesh</h1>
<p align="center">A Linux 3D model viewer that plays skeletons and animations, not just meshes.<br>
Opens <a href="https://www.khronos.org/gltf/">glTF</a>, FBX, OBJ, STL and many other formats.<br>
Look around, then fly through the model.</p>

<p align="center">
  <a href="https://gtk.org">
    <img src="https://img.shields.io/badge/Toolkit-GTK4-4a86cf?logo=gtk&logoColor=white" alt="GTK4">
  </a>
  <a href="https://gnome.pages.gitlab.gnome.org/libadwaita/">
    <img src="https://img.shields.io/badge/Style-libadwaita-3584e4?logo=gnome&logoColor=white" alt="libadwaita">
  </a>
  <a href="https://www.assimp.org/">
    <img src="https://img.shields.io/badge/Models-Assimp-dcc456?logoColor=white" alt="Assimp">
  </a>
</p>

<p align="center">
  <a href="https://github.com/Crimson-Org/crimson-mesh/releases/latest"><b>Download Here!</b></a>
</p>

## Why

A model sent by a friend or found online should open with a double click, without starting a 3D editor.\
Crimson Mesh shows the model with textures, lighting and animation at once.

## About

### Formats

glTF, FBX, OBJ, Collada, STL, PLY, 3DS and many older formats open with their textures and colours.

### Looking around

Orbit, pan and zoom with the mouse, or fly through the model.

### How a model is built

Show the wireframe or the skeleton, and models with only a rig show their bones by themselves.

### Animations

Animations play by themselves, and the bottom bar lets you pause, scrub or pick one.

### Light and backdrop

Switch between lighting setups and backgrounds, and your choice is saved.

## Building

You need a [C compiler](https://gcc.gnu.org/), [make](https://www.gnu.org/software/make/), [pkg-config](https://www.freedesktop.org/wiki/Software/pkg-config/) and the development files for [GTK4](https://gtk.org), [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/), [Assimp](https://www.assimp.org/) and [libepoxy](https://github.com/anholt/libepoxy).\
On [openSUSE](https://www.opensuse.org/) these come from `sudo zypper install gcc make pkg-config gtk4-devel libadwaita-devel assimp-devel libepoxy-devel`.

Steps:
 1. Clone using `git clone --recurse-submodules https://github.com/Crimson-Org/crimson-mesh`
 2. Run `sh .crimson-build/install.sh`

`sh .crimson-build/uninstall.sh` uninstalls Crimson Mesh.
