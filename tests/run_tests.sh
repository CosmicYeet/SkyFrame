#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exe1=$(mktemp /tmp/skyframe_engine_XXXXXX)
exe2=$(mktemp /tmp/skyframe_urls_XXXXXX)
trap 'rm -f "$exe1" "$exe2"' EXIT
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_engine/include tests/test_engine.c components/skyframe_engine/skyframe_engine.c -lm -o "$exe1"
"$exe1"
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_net/include tests/test_urls.c components/skyframe_net/skyframe_urls.c -lm -o "$exe2"
"$exe2"

exe3=$(mktemp /tmp/skyframe_photos_XXXXXX)
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_screens/include tests/test_photos.c components/skyframe_screens/skyframe_photos.c -o "$exe3"
"$exe3"
rm -f "$exe3"

exe4=$(mktemp /tmp/skyframe_controller_XXXXXX)
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_engine/include tests/test_controller.c components/skyframe_engine/skyframe_controller.c components/skyframe_engine/skyframe_engine.c -lm -o "$exe4"
"$exe4"
rm -f "$exe4"
exe5=$(mktemp /tmp/skyframe_image_XXXXXX)
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_screens/include tests/test_image.c components/skyframe_screens/skyframe_image.c -o "$exe5"
"$exe5"
rm -f "$exe5"

exe6=$(mktemp /tmp/skyframe_config_XXXXXX)
cc -std=c11 -Wall -Wextra -Werror -Icomponents/skyframe_config/include tests/test_config.c components/skyframe_config/skyframe_config.c -lm -o "$exe6"
"$exe6"
rm -f "$exe6"

exe7=$(mktemp /tmp/skyframe_today_XXXXXX)
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_engine/include tests/test_today.c components/skyframe_engine/skyframe_today.c -o "$exe7"
"$exe7"
rm -f "$exe7"

exe8=$(mktemp /tmp/skyframe_update_XXXXXX)
cc -std=c11 -Wall -Wextra -Icomponents/skyframe_update/include -Imanaged_components/espressif__cjson/cJSON tests/test_update.c components/skyframe_update/skyframe_update_manifest.c managed_components/espressif__cjson/cJSON/cJSON.c -lm -o "$exe8"
"$exe8"
rm -f "$exe8"
