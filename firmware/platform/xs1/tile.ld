# Link description for one XS1-L16 tile, for tools/xcore/xld.py: pass as @platform/xs1/tile.ld
# (lines are xld.py arguments). Each tile is linked separately; ximage.py joins the tiles.
--entry _start
--ram 0x10000:0x10000          # 64 KB RAM per tile; code, cp, dp and the stack share it
--stack 2048                   # fail the link if less than this is left for the stack
platform/xs1/crt0.s            # placed first: the boot loader jumps to the RAM base
