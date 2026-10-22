.. SPDX-License-Identifier: GPL-2.0+

cfr
===

Synopsis
--------

::

    cfr [list [-v]]
    cfr show <name>
    cfr set <name> <value>
    cfr default <name> | -a
    cfr pending
    cfr save
    cfr discard

Description
-----------

coreboot describes its runtime options in the CFR table, which it passes to
U-Boot with the coreboot tables. The cfr command lists these options and
changes them.

The values are UEFI variables with the coreboot vendor GUID
ceae4c1d-335b-4685-a4a0-fc4a94eea085, one 32-bit variable per option, named
after the option. coreboot reads them on the next boot. For this, U-Boot needs
a UEFI variable store that coreboot can read, e.g. CONFIG_EFI_VARIABLE_SF_STORE
on the coreboot SMMSTORE (CONFIG_SPI_FLASH_COREBOOT_SMMSTORE), and coreboot
needs its option backend for this store (CONFIG_USE_UBOOT_VARIABLE_STORE).

Changes are staged first and only written by `cfr save`.

list
    Shows the options, grouped by the forms of the CFR table, with their
    current values. A value that differs from the default shows the default as
    well. With `-v`, the help text and the possible values are shown too.
    Options marked "inactive" have no effect because of the value of the
    option they depend on.

show
    Shows one option with its help text, value, default and possible values.

set
    Stages a new value. For an enumeration, the value is the name of a choice
    (case does not matter) or its number. For a boolean, it is one of
    0/1, off/on, no/yes, false/true or disabled/enabled. For a number, it must
    be in the range and step of the option.

default
    Stages the removal of the stored value, so the option returns to its
    default. With `-a`, this is done for all options.

pending
    Shows the staged changes.

save
    Writes the staged changes to the UEFI variable store.

discard
    Drops the staged changes.

Example
-------

::

    => cfr
    Memory
      memory_clear         Memory clear and link ECC  Enabled
    Console
      debug_level          Console Log Level          Debug
    => cfr show memory_clear
    memory_clear: Memory clear and link ECC
      Enabled clears all DRAM during memory init and uses ECC on the LPDDR5
      link. Disabled turns both off, which saves about 1.6 s per boot.
      Value:   Enabled (default)
      Default: Enabled
      Values:  Disabled, Enabled
    => cfr set memory_clear disabled
    memory_clear: Enabled -> Disabled (not saved yet)
    => cfr set debug_level 5
    debug_level: Debug -> Notice (not saved yet)
    => cfr pending
      memory_clear         -> Disabled
      debug_level          -> Notice
    => cfr save
    2 option(s) saved; they apply on the next boot
    => cfr
    Memory
      memory_clear         Memory clear and link ECC  Disabled  (default Enabled)
    Console
      debug_level          Console Log Level          Notice  (default Debug)

Configuration
-------------

The cfr command is available if CONFIG_CMD_CFR is enabled.

Return value
------------

The return value $? is 0 (true) on success, 1 (false) if coreboot provides no
CFR table, the option or value is unknown, or saving failed.
