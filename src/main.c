#include "FreeRTOS.h"
#include "task.h"
#include "timeline_scheduler.h"

#include "uart.h"
#include "trace_module.h"

void vTask_A_Function( void * pvParameters )
{
	( void ) pvParameters;
    /* task logic */
}

void vTask_B_Function( void * pvParameters )
{
	( void ) pvParameters;
    /* task logic */
}

void vTask_X_Function( void * pvParameters )
{
	( void ) pvParameters;
    /* task logic */
}

/* Defined as static so it lives for the entire application lifetime.
 * vConfigureScheduler() stores a pointer to this — never put it on
 * the stack of a function that will return.                              */
static const TimelineTaskConfig_t xTaskConfigs[] =
{
    /*  name          function      type            start  end   subframe */
    { "HRT_A",   vTask_A_Function, TASK_TYPE_HARD_RT,  21,  27,    2 },
    { "HRT_B",   vTask_B_Function, TASK_TYPE_HARD_RT,  40,  47,    5 },
    { "SRT_X",   vTask_X_Function, TASK_TYPE_SOFT_RT,   0,   0,    0 },
};

static const TimelineConfig_t xScheduleConfig =
{
    .pxTasks        = xTaskConfigs,
    .uxTaskCount    = 3U,
    .ulMajorFrame_ms = 100U
};

int main(int argc, char **argv)
{

	(void) argc;
	(void) argv;

    /* 1. Hardware initialisation */
    UART_init();

    /* 2. Configure the timeline scheduler — creates all managed tasks */
    vConfigureScheduler( &xScheduleConfig );

    /* 3. Initialise trace — creates the drain task                    */
    vTraceInit();

    /* 4. Start the scheduler — from this point the tick handler fires */
    vTaskStartScheduler();

    /* Never reached */
    for( ; ; );
}

void HardFault_Handler( void )
{
    UART_printf( "HARDFAULT\r\n" );
    for( ; ; );
}