#include "sdkconfig.h"

#if CONFIG_MICCLICKER_STAGE1_SELFTEST
void micclicker_stage1_selftest_run(void);
#else
void micclicker_usb_prototype_start(void);
#endif

void app_main(void)
{
#if CONFIG_MICCLICKER_STAGE1_SELFTEST
    micclicker_stage1_selftest_run();
#else
    micclicker_usb_prototype_start();
#endif
}
