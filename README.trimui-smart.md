# Picoarch for Trimui Smart

First, you'll need the Miyoo Mini (not a typo) docker [toolchain](https://github.com/shauninman/union-miyoomini-toolchain).

Check out this repo into the toolchain's workspace directory and switch to the `trimui-smart` branch.

From the toolchain repo run `make shell` and then in the toolchain shell run `cd picoarch`. 

To build run `make platform=trimui picoarch`.

I haven't done any work getting the individual cores building for the Smart but the cores in [MiniUI](https://github.com/shauninman/MiniUI/releases) compiled for the Miyoo Mini work as-is.

Just copy your compiled `./picoarch` binary and the Miyoo Mini cores into `./trimui/app` then copy the whole `trimui` folder to the root of your SD card. I'm pretty sure that's it.

(I call that package "picosmartch". Which is dumb. Naming things is hard.)