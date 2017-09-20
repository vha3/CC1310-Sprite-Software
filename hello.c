/*
 * I2C communication with LSM9DS1 9-axis IMU
 * Hunter Adams
 * vha3@cornell.edu
 */

/* TI-RTOS Header files */
#include <xdc/std.h>
#include <unistd.h>
#include <stdlib.h>
#include <xdc/runtime/System.h>
#include <xdc/runtime/Error.h>

/* Driver header files */
#include <ti/drivers/I2C.h>
//#include <ti/display/Display.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26XX.h>
#include <ti/drivers/PIN.h>
#include <ti/drivers/pin/PINCC26XX.h>
#include <ti/drivers/Watchdog.h>

/* BIOS Header files */
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Semaphore.h>

/* Example/Board Header files */
#include "Board.h"

/* Other header files */
#include "LSM9DS1.h"
#include "Network_Types.h"
//#include "TRIAD.h"
#include "smartrf_settings/smartrf_settings.h"
#include "easylink/EasyLink.h"

#include <ti/drivers/UART.h>
#include <ti/drivers/uart/UARTCC26XX.h>

/* TX quantities */
#define RFEASYLINKTX_BURST_SIZE         10
#define RFEASYLINKTXPAYLOAD_LENGTH      29

/* Addresses */
#define UNIVERSAL_ADDRESS 0xaa
#define PERSONAL_ADDRESS 0xbb

static uint8_t AddressList[0x10] =
{
		UNIVERSAL_ADDRESS, // First address to match
		PERSONAL_ADDRESS // Second address to match
};

/* Display Handle */
//static Display_Handle display;

/* Pin handles and states*/
static PIN_Handle pinHandle;
static PIN_State pinState;

/* Stuff related to clock */
Void clk0Fxn(UArg arg0);
Clock_Struct clk0Struct;
Clock_Handle clk0Handle;


/*
 * Application button pin configuration table:
 *   - Interrupts are configured to trigger on rising edge.
 */
PIN_Config pinTable[] = {
	CC1310_LAUNCHXL_DIO15  | PIN_INPUT_EN | PIN_PULLDOWN | PIN_IRQ_POSEDGE,
	CC1310_LAUNCHXL_DIO12  | PIN_INPUT_EN | PIN_PULLDOWN | PIN_IRQ_POSEDGE,
	CC1310_LAUNCHXL_DIO22  | PIN_INPUT_EN | PIN_PULLDOWN | PIN_IRQ_POSEDGE,
	CC1310_LAUNCHXL_PIN_RLED | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,
	CC1310_LAUNCHXL_PIN_GLED | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,
    PIN_TERMINATE
};

/* Task structs */
Task_Struct initializationTask;
Task_Struct calibrationTask;
Task_Struct magTask;
Task_Struct gyroTask;
Task_Struct accelTask;
Task_Struct txDataTask;
Task_Struct rxRestartTask;
Task_Struct rxBeaconTask;
Task_Struct gpsTask;

/* Attitude buffer */
//float attitudeBuffer[9];

/* Semaphore structs */
static Semaphore_Struct initSemaphore;
static Semaphore_Handle initSemaphoreHandle;

static Semaphore_Struct calibSemaphore;
static Semaphore_Handle calibSemaphoreHandle;

static Semaphore_Struct magSemaphore;
static Semaphore_Handle magSemaphoreHandle;

static Semaphore_Struct gyroSemaphore;
static Semaphore_Handle gyroSemaphoreHandle;

static Semaphore_Struct accelSemaphore;
static Semaphore_Handle accelSemaphoreHandle;

static Semaphore_Struct txDataSemaphore;
static Semaphore_Handle txDataSemaphoreHandle;

static Semaphore_Struct rxRestartSemaphore;
static Semaphore_Handle rxRestartSemaphoreHandle;

static Semaphore_Struct rxBeaconSemaphore;
static Semaphore_Handle rxBeaconSemaphoreHandle;

static Semaphore_Struct batonSemaphore;
static Semaphore_Handle batonSemaphoreHandle;

/* Make sure we have nice 8-byte alignment on the stack to avoid wasting memory */
#pragma DATA_ALIGN(txDataTaskStack, 8)
static uint8_t initializationTaskStack[400];
static uint8_t calibrationTaskStack[450];
static uint8_t magTaskStack[450];
static uint8_t gyroTaskStack[450];
static uint8_t accelTaskStack[450];
static uint8_t txDataTaskStack[1024];
static uint8_t rxRestartTaskStack[300];
static uint8_t rxBeaconTaskStack[300];
static uint8_t gpsTaskStack[800];

int goodToGo = 0;

/* List for holding neighbors */
int numConnections = 0;
uint8_t Connections[MAXNEIGHBORS] = {};

UART_Handle uart;
UART_Params uartParams;

/* ###########################################################
 * Some Bit Manipulation Functions
 */
//static uint16_t seqNumber;

uint8_t sign(int16_t x)
{
	if(x>0){
		return 1;
	}
	else{
		return 0;
	}
}

uint8_t lowerPart(int16_t x){
	return (x & 0xff);
}

uint8_t upperPart(int16_t x){
	return (x >> 8);
}
/*
 * ###########################################################
 */
Watchdog_Handle watchdogHandle;
void wdtSetup()
{
	Watchdog_Params params;
	/* Call board init functions */
	Watchdog_init();
	/* Create and enable a Watchdog with resets disabled */
	Watchdog_Params_init(&params);
	params.resetMode = Watchdog_RESET_ON; //generate Reboot interrupt
	watchdogHandle = Watchdog_open(Board_WATCHDOG0, &params);
	Watchdog_setReload(watchdogHandle, 3000000); //set WDT period
}

/* RX callback function */
EasyLink_RxPacket globalPacket = {0};
void rxDoneCb(EasyLink_RxPacket * rxPacket, EasyLink_Status status)
{
    if (status == EasyLink_Status_Success)
    {
        /* Toggle LED2 to indicate RX */
        PIN_setOutputValue(pinHandle, CC1310_LAUNCHXL_PIN_RLED,
        		!PIN_getOutputValue(CC1310_LAUNCHXL_PIN_RLED));
        globalPacket.dstAddr[0] = rxPacket->dstAddr[0];
        globalPacket.rssi = rxPacket->rssi;
        globalPacket.absTime = rxPacket->absTime;
        globalPacket.rxTimeout = rxPacket->rxTimeout;
        globalPacket.len = rxPacket->len;
        int i=0;
        while(i < globalPacket.len){
        		globalPacket.payload[i] = rxPacket->payload[i];
        		i += 1;
        }
        if (globalPacket.payload[0] == BEACON){
        		Semaphore_post(rxBeaconSemaphoreHandle);
        }
        else {
        		Semaphore_post(rxRestartSemaphoreHandle);
        }

    }
    else if(status == EasyLink_Status_Aborted)
    {
        /* Toggle LED1 to indicate command aborted */
        PIN_setOutputValue(pinHandle, CC1310_LAUNCHXL_PIN_GLED,
        		!PIN_getOutputValue(CC1310_LAUNCHXL_PIN_GLED));
        Semaphore_post(rxRestartSemaphoreHandle);
    }
    else
    {
        /* Toggle LED1 and LED2 to indicate error */
        PIN_setOutputValue(pinHandle, CC1310_LAUNCHXL_PIN_GLED,
        		!PIN_getOutputValue(CC1310_LAUNCHXL_PIN_GLED));
        PIN_setOutputValue(pinHandle, CC1310_LAUNCHXL_PIN_RLED,
        		!PIN_getOutputValue(CC1310_LAUNCHXL_PIN_RLED));
    }
}

Void clk0Fxn(UArg arg0)
{
	if(goodToGo){
		Watchdog_clear(watchdogHandle);
		Semaphore_post(txDataSemaphoreHandle);
	}
}

/*
 * As far as I can tell, it's not strictly necessary to have
 * arguments for these functions. Having them prevents a bunch
 * of warnings, however, so I'm keeping them for now.
 */
Void initializationTaskFunc(UArg arg0, UArg arg1)
{
    while (1) {
    		Semaphore_pend(initSemaphoreHandle,BIOS_WAIT_FOREVER);

        uint16_t workpls = LSM9DS1begin();
        configInt(XG_INT1, INT_DRDY_G, INT_ACTIVE_HIGH, INT_PUSH_PULL);
        configInt(XG_INT2, INT_DRDY_XL, INT_ACTIVE_HIGH, INT_PUSH_PULL);

        Semaphore_post(calibSemaphoreHandle);
    }
}

Void calibrationTaskFunc(UArg arg0, UArg arg1)
{
	while (1) {
		Semaphore_pend(calibSemaphoreHandle, BIOS_WAIT_FOREVER);
		calibrate(1);
//		calibrateMag(1);
		/* getMagInitial is only required if you're calibrating for the computer attitude */
		getMagInitial();
		goodToGo += 1;
		readGyro();
		readAccel();
		readMag();
	}
}

Void magTaskFunc(UArg arg0, UArg arg1)
{
    while (1) {
    		Semaphore_pend(magSemaphoreHandle, BIOS_WAIT_FOREVER);
    		Semaphore_pend(batonSemaphoreHandle, BIOS_WAIT_FOREVER);
    		if(goodToGo){
    			readMag();
    		}
    		Semaphore_post(batonSemaphoreHandle);
    }
}

Void gyroTaskFunc(UArg arg0, UArg arg1)
{
    while (1) {
    		Semaphore_pend(gyroSemaphoreHandle, BIOS_WAIT_FOREVER);
    		Semaphore_pend(batonSemaphoreHandle, BIOS_WAIT_FOREVER);
    		if(goodToGo){
    			readGyro();
    		}
    		Semaphore_post(batonSemaphoreHandle);
    }
}

Void accelTaskFunc(UArg arg0, UArg arg1)
{
    while (1) {
    		Semaphore_pend(accelSemaphoreHandle, BIOS_WAIT_FOREVER);
    		Semaphore_pend(batonSemaphoreHandle, BIOS_WAIT_FOREVER);
    		if(goodToGo){
    			readAccel();
    		}
    		Semaphore_post(batonSemaphoreHandle);
    }
}

Void txDataTaskFunc(UArg arg0, UArg arg1)
{
	EasyLink_init(EasyLink_Phy_Custom);
	EasyLink_setRfPwr(12);
	EasyLink_enableRxAddrFilter((uint8_t*)&AddressList, 1, 2);

	while(1) {
		Semaphore_pend(txDataSemaphoreHandle, BIOS_WAIT_FOREVER);
		Semaphore_pend(batonSemaphoreHandle, BIOS_WAIT_FOREVER);
		if(goodToGo){
			EasyLink_abort();
			EasyLink_TxPacket txPacket =  { {0}, 0, 0, {0} };

			txPacket.payload[0] = BEACON;
			txPacket.payload[1] = PERSONAL_ADDRESS;

//			txPacket.payload[0] = (uint8_t)(seqNumber >> 8);
//			txPacket.payload[1] = (uint8_t)(seqNumber++);
//			txPacket.payload[2] = sign(ax);
//			txPacket.payload[3] = upperPart(ax);
//			txPacket.payload[4] = lowerPart(ax);
//			txPacket.payload[5] = sign(ay);
//			txPacket.payload[6] = upperPart(ay);
//			txPacket.payload[7] = lowerPart(ay);
//			txPacket.payload[8] = sign(az);
//			txPacket.payload[9] = upperPart(az);
//			txPacket.payload[10] = lowerPart(az);
//
//			txPacket.payload[11] = sign(gx);
//			txPacket.payload[12] = upperPart(gx);
//			txPacket.payload[13] = lowerPart(gx);
//			txPacket.payload[14] = sign(gy);
//			txPacket.payload[15] = upperPart(gy);
//			txPacket.payload[16] = lowerPart(gy);
//			txPacket.payload[17] = sign(gz);
//			txPacket.payload[18] = upperPart(gz);
//			txPacket.payload[19] = lowerPart(gz);
//
//			txPacket.payload[20] = sign(mx);
//			txPacket.payload[21] = upperPart(mx);
//			txPacket.payload[22] = lowerPart(mx);
//			txPacket.payload[23] = sign(my);
//			txPacket.payload[24] = upperPart(my);
//			txPacket.payload[25] = lowerPart(my);
//			txPacket.payload[26] = sign(mz);
//			txPacket.payload[27] = upperPart(mz);
//			txPacket.payload[28] = lowerPart(mz);
//			for (i = 0; i < RFEASYLINKTXPAYLOAD_LENGTH; i++)
//			{
//			  txPacket.payload[i] = message[i];
//			}
	        txPacket.len = RFEASYLINKTXPAYLOAD_LENGTH;
	        txPacket.dstAddr[0] = 0xaa;
	        txPacket.absTime = 0;
	        EasyLink_Status result = EasyLink_transmit(&txPacket);

	        if (result == EasyLink_Status_Success)
			{
				/* Toggle LED1 to indicate TX */
//				PIN_setOutputValue(pinHandle, Board_PIN_LED1,!PIN_getOutputValue(Board_PIN_LED1));
			}
			else
			{
				/* Toggle LED1 and LED2 to indicate error */
				PIN_setOutputValue(pinHandle, Board_PIN_LED1,!PIN_getOutputValue(Board_PIN_LED1));
				PIN_setOutputValue(pinHandle, Board_PIN_LED2,!PIN_getOutputValue(Board_PIN_LED2));
			}
	        Semaphore_post(rxRestartSemaphoreHandle);
		}
		Semaphore_post(batonSemaphoreHandle);
	}
}

Void rxRestartFunc(UArg arg0, UArg arg1)
{
    while(1) {
    		Semaphore_pend(rxRestartSemaphoreHandle, BIOS_WAIT_FOREVER);
    		EasyLink_receiveAsync(rxDoneCb, 0);
    }
}

Void rxBeaconFunc(UArg arg0, UArg arg1)
{
    while(1) {
    		Semaphore_pend(rxBeaconSemaphoreHandle, BIOS_WAIT_FOREVER);
    		uint8_t senderAddress = globalPacket.payload[1];
    		if (numConnections == 0){
    			Connections[0] = senderAddress;
    			numConnections += 1;
//    			Display_printf(display, 0, 0, "%02x", Connections[0]);
    			Semaphore_post(rxRestartSemaphoreHandle);
    		}
    		else {
    			int index = 0;
    			int similarities = 0;
    			while (index < numConnections){
    				if (Connections[index] == senderAddress){
    					similarities = 1;
    				}
    				index += 1;
    			}
    			if(similarities){
    				Semaphore_post(rxRestartSemaphoreHandle);
    			}
    			else{
    				if (numConnections < MAXNEIGHBORS){
						Connections[numConnections] = senderAddress;
//						Display_printf(display, 0, 0, "%02x", Connections[numConnections]);
						numConnections += 1;
						Semaphore_post(rxRestartSemaphoreHandle);
    				}
    				else{
    					Semaphore_post(rxRestartSemaphoreHandle);
    				}
    			}
    		}
    }
}

Void gpsFunc(UArg arg0, UArg arg1)
{
    UART_Params_init(&uartParams);
    uartParams.writeDataMode = UART_DATA_BINARY;
    uartParams.readDataMode = UART_DATA_TEXT;
    uartParams.readReturnMode = UART_RETURN_FULL;
    uartParams.readEcho = UART_ECHO_OFF;
    uartParams.baudRate = 9600;

    uart = UART_open(Board_UART0, &uartParams);

	if (uart == NULL) {
		/* UART_open() failed */
		while (1);
	}
	UART_control(uart, UARTCC26XX_CMD_RETURN_PARTIAL_ENABLE, NULL);
	char        input[256] = {0};
	const char 	newlinePrompt[] = "\r\n";
	while (1) {
		if(goodToGo){
			int numBytes = UART_read(uart, &input, 256);
			UART_write(uart, &input, numBytes);
			UART_write(uart, newlinePrompt, sizeof(newlinePrompt));
			UART_control(uart, UARTCC26XX_CMD_RX_FIFO_FLUSH, NULL);
			Task_yield();
		}
	}
}


void pinCallback(PIN_Handle handle, PIN_Id pinId) {
//    uint32_t currVal = 0;
	switch (pinId) {
		case CC1310_LAUNCHXL_DIO12:
//			currVal =  PIN_getOutputValue(Board_PIN_LED0);
//			PIN_setOutputValue(pinHandle, Board_PIN_LED0, !currVal);
			Semaphore_post(gyroSemaphoreHandle);
			break;

		case CC1310_LAUNCHXL_DIO15:
//			currVal =  PIN_getOutputValue(Board_PIN_LED1);
//			PIN_setOutputValue(pinHandle, Board_PIN_LED1, !currVal);
			Semaphore_post(magSemaphoreHandle);
			break;

		case CC1310_LAUNCHXL_DIO22:
//			currVal =  PIN_getOutputValue(Board_PIN_LED1);
//			PIN_setOutputValue(pinHandle, Board_PIN_LED1, !currVal);
			Semaphore_post(accelSemaphoreHandle);
			break;

		default:
			/* Do nothing */
			break;
	}
}

/*
 *  ======== main ========
 *
 */
int main(void)
{

	Clock_Params clkParams;

	/* Initialize TI drivers */
    Board_initGeneral();
//    Display_init();
    I2C_init();
    PIN_init(pinTable);
    UART_init();

    wdtSetup();

    /* Open Display */
//    display = Display_open(Display_Type_UART, NULL);
//    if (display == NULL){
//    		while(1);
//    }

    Clock_Params_init(&clkParams);
	clkParams.period = 500000/Clock_tickPeriod;
	clkParams.startFlag = TRUE;

	/* Construct a periodic Clock Instance */
	Clock_construct(&clk0Struct, (Clock_FuncPtr)clk0Fxn,
					5000/Clock_tickPeriod, &clkParams);

    	/* Open I2C connection to LSM9DS1 */
    LSM9DS1init();
    initI2C();


    /* Setup/create tasks */
    Task_Params task_params;
    Task_Params_init(&task_params);
    task_params.stackSize = 400;
    task_params.priority = 4;
    task_params.stack = &initializationTaskStack;
    Task_construct(&initializationTask, initializationTaskFunc,
    		           &task_params, NULL);

    task_params.stackSize = 450;
    task_params.priority = 3;
    task_params.stack = &calibrationTaskStack;
    Task_construct(&calibrationTask, calibrationTaskFunc,
    				   &task_params, NULL);

    	task_params.stackSize = 450;
    task_params.priority = 2;
    task_params.stack = &magTaskStack;
    Task_construct(&magTask, magTaskFunc,
    		           &task_params, NULL);

    task_params.stackSize = 450;
    task_params.priority = 2;
    task_params.stack = &gyroTaskStack;
    Task_construct(&gyroTask, gyroTaskFunc,
    		           &task_params, NULL);

    task_params.stackSize = 450;
    task_params.priority = 2;
    task_params.stack = &accelTaskStack;
    Task_construct(&accelTask, accelTaskFunc,
    		           &task_params, NULL);

    task_params.stackSize = 1024;
    task_params.priority = 2;
	task_params.stack = &txDataTaskStack;
	Task_construct(&txDataTask, txDataTaskFunc,
				   &task_params, NULL);

    task_params.stackSize = 300;
    task_params.priority = 2;
	task_params.stack = &rxRestartTaskStack;
	Task_construct(&rxRestartTask, rxRestartFunc,
				   &task_params, NULL);

    task_params.stackSize = 300;
    task_params.priority = 2;
	task_params.stack = &rxBeaconTaskStack;
	Task_construct(&rxBeaconTask, rxBeaconFunc,
				   &task_params, NULL);

	task_params.stackSize = 800;
	task_params.priority = 1;
	task_params.stack = &gpsTaskStack;
	Task_construct(&gpsTask, gpsFunc,
				   &task_params, NULL);


    /* Create Semaphores */
    Semaphore_construct(&initSemaphore, 1, NULL);
    initSemaphoreHandle = Semaphore_handle(&initSemaphore);

    Semaphore_construct(&calibSemaphore, 0, NULL);
    calibSemaphoreHandle = Semaphore_handle(&calibSemaphore);

    Semaphore_construct(&magSemaphore, 0, NULL);
    magSemaphoreHandle = Semaphore_handle(&magSemaphore);

    Semaphore_construct(&gyroSemaphore, 0, NULL);
    gyroSemaphoreHandle = Semaphore_handle(&gyroSemaphore);

    Semaphore_construct(&accelSemaphore, 0, NULL);
    accelSemaphoreHandle = Semaphore_handle(&accelSemaphore);

	Semaphore_construct(&txDataSemaphore, 0, NULL);
	txDataSemaphoreHandle = Semaphore_handle(&txDataSemaphore);

	Semaphore_construct(&rxRestartSemaphore, 0, NULL);
	rxRestartSemaphoreHandle = Semaphore_handle(&rxRestartSemaphore);

	Semaphore_construct(&rxBeaconSemaphore, 0, NULL);
	rxBeaconSemaphoreHandle = Semaphore_handle(&rxBeaconSemaphore);

    Semaphore_construct(&batonSemaphore, 1, NULL);
    batonSemaphoreHandle = Semaphore_handle(&batonSemaphore);


    pinHandle = PIN_open(&pinState, pinTable);
	if(!pinHandle) {
		/* Error initializing button pins */
		while(1);
	}

    /* Setup callback for button pins */
    if (PIN_registerIntCb(pinHandle, &pinCallback) != 0) {
        /* Error registering button callback function */
        while(1);
    }

    /* Start kernel. */
    BIOS_start();

    return (0);
}
