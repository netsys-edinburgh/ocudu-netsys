srsRAN - Upper physical layer tap plugin
================

This project is a C++ plugin designed to integrate with srsRAN, and exposes the upper physical layer symbols to an external
processor.

## Overview

The plugin includes a decorator for the physical layer receive symbol handler. This is the point in the uplink
processing
chain in which the symbols are passed to the upper physical layer for processing. This decorator calls an external
uplink processor object
that can be used to insert custom processing algorithms.

## How to add your own processing algorithms

By default, a dummy external processor is included in the plugin. This processor scales the received symbols by a
constant, and serves as an implementation example.
To quickly test the plugin, the `process` method of this dummy processor can be extended to implement custom processing
logic.

The external UL processor is instantiated following the factory design pattern, allowing multiple implementations to be
added.
All external UL processors must implement the interface defined in `/include/external_ul_processor.h`. You can add your
own implementation of the external UL processor
in `/lib/external_processors/`. In order to instantiate it, a factory must be defined in
`/lib/external_processors/external_processor_factories.cpp`. This factory must implement the interface defined in
`/include/external_processor_factories.h`. A factory creation function must also be declared in this file, and defined
in `/lib/external_processor_factories.cpp`.

To instantiate your own implementation instead of the dummy processor, replace the call to the factory creation function
in the `upper_phy_rx_symbol_handler_tap_factory_impl` constructor:

```cpp
// [EXTERNAL CODE INSERTION START] Use your own external UL processor factory creation function here.

// Create the external UL processor factory.
processor_factory = create_external_ul_procesor_dummy_factory(nof_rb_, nof_ports_, processor_arguments_);

// [EXTERNAL CODE INSERTION END]
```

All the sections of the code that may be modifed to extend the plugin as described are wrapped in comments:

```cpp
// [EXTERNAL CODE INSERTION START]
// [EXTERNAL CODE INSERTION END]
```

If you need modifications in other parts of the code, it is best to open issues and engage with the srsRAN team, as
modifications in unmarked sections may lead to incompatibilities with the srsRAN codebase.

## Configuration of the external UL processor

Currently, two main features can be configured using the `phy_tap_arguments` parameter:

- Enable processing of quiet (i.e., unallocated) UL symbols, by setting `enable_quiet_processing=true`. By default, this is disabled.
- Set a specific log level using `log_level=$LEVEL$`. Supported log levels (`$LEVEL$` values) are (from lowest to highest detail): `none`, `error`, `warning`, `info`, and `debug`.

The following example shows an excerpt from a gNB configuration file (yml) that enables quiet UL symbol processing and sets the log level to `warning`:

```
expert_phy:
  enable_phy_tap: true
  phy_tap_arguments: enable_quiet_processing=true,log_level=warning
```

Alternatively, those same configuration parameters can be passed through the console when starting the gNB binary by adding `expert_phy --enable_phy_tap=true --enable_quiet_processing=true,log_level=warning`
