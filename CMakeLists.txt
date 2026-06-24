# Copyright (c) 2026 The ZMK Behavior Dynamic Macro Contributors
# SPDX-License-Identifier: MIT

target_include_directories(app PRIVATE include)

if(CONFIG_ZMK_ZBDM)
  target_sources(app PRIVATE src/behavior_zbdm.c)

  if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources(app PRIVATE src/zbdm.c)
    target_sources_ifdef(CONFIG_ZMK_ZBDM_TEST_SETTINGS app PRIVATE src/zbdm_test.c)
  endif()
endif()
