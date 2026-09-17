# CLI design guidelines

- CLI schema creation files (ending in `*_schema.cpp`) can make basic, individual parameter value validation checks (e.g. `check(CLI::Range(-90.0, 90.0))`) to ensure a config value is within the accepted range
- validator files (ending in `*_validator.cpp`) are used to make more advanced config checks, usually involving cross-checking the values of different CLI config parameters to ensure consistency. Do not use validator files to make checks that the CLI schema files can do.
- translator files (ending in `*_translators.cpp`) convert an already validated CLI config into the internal configuration types. Never place config validation in them: the checks belong in the schema or validator files, and a translator may assume the config is valid.
