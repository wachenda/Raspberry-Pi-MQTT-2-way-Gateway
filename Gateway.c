/*

Original Code RFM69 Gateway RFM69 pushing the data to the mosquitto server
by Alexandre Bouillot

Modified by David Wachenschwanz to provide 2-way communciation between MQTT and nodes

License:  CC-BY-SA, https://creativecommons.org/licenses/by-sa/2.0/
Date:  2015-06-12
File: Gateway.c

This sketch receives RFM wireless data and forwards it to Mosquitto relay
It also subscripe to Mosquitto Topics starting with RFM/<network_number> followed  by /<node_id>
The message is parsed and put bak to the same payload structure as the one received from teh nodes


Adjust network configuration to your setup in the file networkconfig.h
*/


#define VERSION "GW Rev053016"

//general --------------------------------
#define SERIAL_BAUD   115200
#ifdef DAEMON
#define LOG(...) do { syslog(LOG_INFO, __VA_ARGS__); } while (0)
#define LOG_E(...) do { syslog(LOG_ERR, __VA_ARGS__); } while (0)
#else
#ifdef DEBUG
#define DEBUG1(expression)  fprintf(stderr, expression)
#define DEBUG2(expression, arg)  fprintf(stderr, expression, arg)
#define DEBUGLN1(expression)  
#define LOG(...) do { printf(__VA_ARGS__); } while (0)
#define LOG_E(...) do { printf(__VA_ARGS__); } while (0)
#else
#define DEBUG1(expression)
#define DEBUG2(expression, arg)
#define DEBUGLN1(expression)
#define LOG(...)
#define LOG_E(...)
#endif
#endif

//RFM69  ----------------------------------
#include "rfm69.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "networkconfig.h"


RFM69 *rfm69;

typedef struct {		
	unsigned long messageWatchdog;
	unsigned long messageSent;
	unsigned long messageReceived;
	unsigned long ackRequested;
	
	unsigned long ackReceived;
	unsigned long ackMissed;
	
	unsigned long ackCount;
} 
Stats;
Stats theStats;

typedef struct {
	uint8_t networkId;
	uint8_t nodeId;
	uint8_t frequency; // RF69_433MHZ RF69_868MHZ RF69_915MHZ
	uint8_t keyLength; // set to 0 for no encryption
	char key[16];
	bool isRFM69HW;
	bool promiscuousMode;
	unsigned long messageWatchdogDelay; // maximum time between two message before restarting radio module
	}
Config;
Config theConfig;

// Mosquitto---------------
#include <mosquitto.h>

/* How many seconds the broker should wait between sending out
* keep-alive messages. */
#define KEEPALIVE_SECONDS 60
/* Hostname and port for the MQTT broker. */
#define BROKER_HOSTNAME "localhost"
#define BROKER_PORT 1883

#define MQTT_ROOT "RFM"
#define MQTT_CLIENT_ID "YOUR_CLIENT_ID"  // Need to change for your setup
#define MQTT_RETRY 500

#define NODEID 1 

int sendMQTT = 0;

#pragma pack(push,1)
typedef struct {        // Radio packet structure max 66 bytes
    short nodeID;       // node identifier
    short devID;        // device identifier 0 is node; 31 is temperature, 32 is humidity
    short cmd;        // read or write
    int    intVal;       // integer payload
    float   fltVal;       // floating payload
    char    payLoad[32];      // char array payload
} Message;
#pragma pack(pop)

Message mes;

typedef struct {		
	short           nodeID; 
	short			sensorID;
	unsigned long   var1_usl; 
	float           var2_float; 
	float			var3_float;	
} 
Payload;
Payload theData;

typedef struct {
	short           nodeID;
	short			sensorID;		
	unsigned long   var1_usl;
	float           var2_float;
	float			var3_float;		//
	int             var4_int;
}
SensorNode;
SensorNode sensorNode;

static void die(const char *msg);
static long millis(void);
static void hexDump (char *desc, void *addr, int len, int bloc);

static int initRfm(RFM69 *rfm);

static bool set_callbacks(struct mosquitto *m);
static bool connect(struct mosquitto *m);
static int run_loop(struct mosquitto *m);

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val);
static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val);
static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val);

static void uso(void) {
	fprintf(stderr, "Use:\n Simply use it without args :D\n");
	exit(1);
}

static void sendMsg(short);
static void processPacket(struct mosquitto *);
static int strtoi(const char *, int *);
static int mystrncmp(const char *, const char *, size_t);
static bool IsREAD(char *);
static bool IsON(char *);
static bool IsOFF(char *);

int error;
short dest;	// destination nose for radio packet
short DID;	// Device ID
char  buff_topic[30];       // MQTT publish topic string
char  buff_mess[32];        // MQTT publish message string
bool mqttToSend = false;
bool  IntMess, RealMess, StatMess, StrMess; // types of messages
long  lastMinute = -1;      // timestamp last minute
long  upTime = 0;       // uptime in minutes
bool  Rstat = false;        // radio indicator flag
bool  respNeeded = false;     // MQTT message flag in case of radio connection failure

int main(int argc, char* argv[]) {
	if (argc != 1) uso();

#ifdef DAEMON
	//Adapted from http://www.netzmafia.de/skripten/unix/linux-daemon-howto.html
	pid_t pid, sid;

	openlog("Gatewayd", LOG_PID, LOG_USER);

	pid = fork();
	if (pid < 0) {
		LOG_E("fork failed");
		exit(EXIT_FAILURE);
	}
	/* If we got a good PID, then
		 we can exit the parent process. */
	if (pid > 0) {
		LOG("Child spawned, pid %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		LOG_E("setsid failed");
		exit(EXIT_FAILURE);
	}
        
	/* Change the current working directory */
	if ((chdir("/")) < 0) {
	  LOG_E("chdir failed");
	  exit(EXIT_FAILURE);
	}
        
	/* Close out the standard file descriptors */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
#endif //DAEMON

	// Mosquitto ----------------------
	struct mosquitto *m = mosquitto_new(MQTT_CLIENT_ID, true, null);
	if (m == NULL) { die("init() failure\n"); }

	if (!set_callbacks(m)) { die("set_callbacks() failure\n"); }
	if (!connect(m)) { die("connect() failure\n"); }

	//RFM69 ---------------------------
	theConfig.networkId = NWC_NETWORK_ID;
	theConfig.nodeId = NWC_NODE_ID;
	theConfig.frequency = NWC_FREQUENCY;
	theConfig.keyLength = NWC_KEY_LENGTH;
	memcpy(theConfig.key, NWC_KEY, NWC_KEY_LENGTH);
	theConfig.isRFM69HW = NWC_RFM69H;
	theConfig.promiscuousMode = NWC_PROMISCUOUS_MODE;
	theConfig.messageWatchdogDelay = NWC_WATCHDOG_DELAY; // 1800 seconds (30 minutes) between two messages 

	rfm69 = new RFM69();
	rfm69->initialize(theConfig.frequency,theConfig.nodeId,theConfig.networkId);
	initRfm(rfm69);

	// Mosquitto subscription ---------
	char subsciptionMask[128];
	//sprintf(subsciptionMask, "%s/%03d/#", MQTT_ROOT, theConfig.networkId);
    sprintf(subsciptionMask, "home/rfm_gw/sb/#");
	LOG("Subscribe to Mosquitto topic: %s\n", subsciptionMask);
	mosquitto_subscribe(m, NULL, subsciptionMask, 0);
	
    LOG("setup complete\n");
	return run_loop(m);
}  // end of setup

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct mosquitto *m) {
	int res;
	long lastMess; 
	for (;;) {
		res = mosquitto_loop(m, 10, 1);

        if (lastMinute != (millis()/60000)) {     // another minute passed ?
            lastMinute = millis()/60000;
            upTime++;
        }


		// No messages have been received withing MESSAGE_WATCHDOG interval
		if (millis() > lastMess + theConfig.messageWatchdogDelay) {
			LOG("=== Message WatchDog ===\n");
			theStats.messageWatchdog++;
			// re-initialise the radio
			initRfm(rfm69);
			// reset watchdog
			lastMess = millis();
		}

        if (mqttToSend) {sendMsg(dest);}    // send MQTT instruction packets over the radio network

        if(rfm69->receiveDone())
        {
            lastMess = millis();
            theStats.messageReceived++;
            processPacket(m);
        }

/*
		if (rfm69->receiveDone()) {
			// record last message received time - to compute radio watchdog
			lastMess = millis();
			theStats.messageReceived++;
			
			// store the received data localy, so they can be overwited
			// This will allow to send ACK immediately after
			uint8_t data[RF69_MAX_DATA_LEN]; // recv/xmit buf, including header & crc bytes
			uint8_t dataLength = rfm69->DATALEN;
			memcpy(data, (void *)rfm69->DATA, dataLength);
			uint8_t theNodeID = rfm69->SENDERID;
			uint8_t targetID = rfm69->TARGETID; // should match _address
			uint8_t PAYLOADLEN = rfm69->PAYLOADLEN;
			uint8_t ACK_REQUESTED = rfm69->ACK_REQUESTED;
			uint8_t ACK_RECEIVED = rfm69->ACK_RECEIVED; // should be polled immediately after sending a packet with ACK request
			int16_t RSSI = rfm69->RSSI; // most accurate RSSI during reception (closest to the reception)

			if (ACK_REQUESTED  && targetID == theConfig.nodeId) {
				// When a node requests an ACK, respond to the ACK
				// but only if the Node ID is correct
				theStats.ackRequested++;
				rfm69->sendACK();
				
				if (theStats.ackCount++%3==0) {
					// and also send a packet requesting an ACK (every 3rd one only)
					// This way both TX/RX NODE functions are tested on 1 end at the GATEWAY

					usleep(3000);  //need this when sending right after reception .. ?
					theStats.messageSent++;
					if (rfm69->sendWithRetry(theNodeID, "ACK TEST", 8)) { // 3 retry, over 200ms delay each
						theStats.ackReceived++;
						LOG("Pinging node %d - ACK - ok!", theNodeID);
					}
					else {
						theStats.ackMissed++;
						LOG("Pinging node %d - ACK - nothing!", theNodeID);
					}
				}
			}//end if radio.ACK_REQESTED
	
			LOG("[%d] to [%d] ", theNodeID, targetID);

			if (dataLength != sizeof(Payload)) {
				LOG("Invalid payload received, not matching Payload struct! %d - %d\r\n", dataLength, sizeof(Payload));
				hexDump(NULL, data, dataLength, 16);		
			} else {
				theData = *(Payload*)data; //assume radio.DATA actually contains our struct and not something else

				//save it for mosquitto:
				sensorNode.nodeID = theData.nodeID;
				sensorNode.sensorID = theData.sensorID;
				sensorNode.var1_usl = theData.var1_usl;
				sensorNode.var2_float = theData.var2_float;
				sensorNode.var3_float = theData.var3_float;
				sensorNode.var4_int = RSSI;

				LOG("Received Node ID = %d Device ID = %d Time = %d  RSSI = %d var2 = %f var3 = %f\n",
					sensorNode.nodeID,
					sensorNode.sensorID,
					sensorNode.var1_usl,
					sensorNode.var4_int,
					sensorNode.var2_float,
					sensorNode.var3_float
				);
				if (sensorNode.nodeID == theNodeID)
					sendMQTT = 1;
				else {
					hexDump(NULL, data, dataLength, 16);
				}
			}  
		} //end if radio.receive
*/
		if (sendMQTT == 1) {
			//send var1_usl
			MQTTSendULong(m, sensorNode.nodeID, sensorNode.sensorID, 1, sensorNode.var1_usl);

			//send var2_float
			MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 2, sensorNode.var2_float);

			//send var3_float
			MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 3, sensorNode.var3_float);

			//send var4_int, RSSI
			MQTTSendInt(m, sensorNode.nodeID, sensorNode.sensorID, 4, sensorNode.var4_int);

			sendMQTT = 0;
		}//end if sendMQTT
	}

	mosquitto_destroy(m);
	(void)mosquitto_lib_cleanup();

	if (res == MOSQ_ERR_SUCCESS) {
		return 0;
	} else {
		return 1;
	}
}

static void sendMsg(short target)
{
    Rstat = true;
    int i = 5;

#if DEBUG
    printf("Sending Msg to: %d: %d %d %d %d ",target,mes.nodeID, mes.devID, mes.cmd, mes.intVal);
    printf("%.2f %s\n",mes.fltVal,mes.payLoad);
#endif

    while(respNeeded && i>0)
    {
        if(rfm69->sendWithRetry(target, (const void *)(&mes), sizeof(mes)))
        {
            respNeeded = false;
        } else usleep(10000);
        i--;
    }

    if(mqttToSend) mqttToSend = false;

}

static void processPacket(struct mosquitto *m)
{
    uint8_t ACK_REQUESTED;
    uint8_t theNodeID;
    int16_t RSSI;

//    Rstat = true;
    
    if(rfm69->DATALEN != sizeof(mes)){
        LOG("inv msg strct\n");
        return;
    }
    else
    {
        ACK_REQUESTED = rfm69->ACK_REQUESTED;
        mes = *(Message*)rfm69->DATA;
        theNodeID = rfm69->SENDERID;
        RSSI = rfm69->RSSI;

        sprintf(buff_topic, "home/rfm_gw/nb/node%02d/dev%02d",theNodeID,mes.devID);
#ifdef DEBUG
        printf("%d, %d, %d, %ld, %.3f, %s\n",mes.nodeID, mes.devID, mes.cmd, mes.intVal, mes.fltVal, mes.payLoad);
        printf("%s\n",buff_topic);
#endif
    }
    DID = mes.devID;
    IntMess = (DID==0 || DID==1 || DID==7 || DID==9 || (DID>=64 && DID<72));  // Integer in payload message
    RealMess = (DID==4 || (DID>=48 && DID <64));          // Float in payload message
    StatMess = (DID==5 || DID==6 || DID==8 || (DID>=16 && DID <32) || (DID>=40 && DID <48));    // Status in payload message
    StrMess = (DID==3 || DID==72);      // String in payload

    if(IntMess) 
    {              // send integer value load
        sprintf(buff_mess, "%d",mes.intVal);
    }

    if(RealMess) 
    {             // send decimal value
        sprintf(buff_mess,"%.2f",mes.fltVal);
    }

    if(StatMess) 
    {             // put status in payload
        if (mes.intVal == 1 )sprintf(buff_mess, "ON");
        if (mes.intVal == 0 )sprintf(buff_mess, "OFF");
    }

    if(StrMess) {
        int i; 
        for (i=0; i<32; i++)
        { 
            buff_mess[i] = (mes.payLoad[i]); 
        }
    } 

    switch (mes.devID)          
    {
        case (2):             // RSSI value
        { 
            //sprintf(buff_mess, "%d", RSSI);
            sprintf(buff_mess, "%d",mes.intVal);
        }
        break;
        case (92):              // invalid device message
        { 
            sprintf(buff_mess, "NODE %d invalid device %d", mes.nodeID, mes.intVal);
        }
        break;
        case(98):
        {
            sprintf(buff_mess,"%s",mes.payLoad);
        }
        break;
        case (99):              // wakeup message
        { 
            sprintf(buff_mess, "NODE %d WAKEUP", mes.nodeID);
        }
        break;
    } // end switch

#ifdef DEBUG
    printf("MQTT msg: %s: %s\n",buff_topic,buff_mess);
#endif

    mosquitto_publish(m, 0, &buff_topic[0], strlen(buff_mess), buff_mess, 0, false);

    if(ACK_REQUESTED)  
    {
        theStats.ackRequested++;
        rfm69->sendACK();
    }
}

static int initRfm(RFM69 *rfm) {
	rfm->restart(theConfig.frequency,theConfig.nodeId,theConfig.networkId);
	if (theConfig.isRFM69HW)
		rfm->setHighPower(); //uncomment only for RFM69HW!
	if (theConfig.keyLength)
		rfm->encrypt(theConfig.key);
	rfm->promiscuous(theConfig.promiscuousMode);
	LOG("Listening at %d Mhz...\n", theConfig.frequency==RF69_433MHZ ? 433 : theConfig.frequency==RF69_868MHZ ? 868 : 915);
}

/* Fail with an error message. */
static void die(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(1);
}

static long millis(void) {
	struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((tv.tv_sec) * 1000 + tv.tv_usec/1000.0) + 0.5;
	}

	
/* Binary Dump utility function */
#define MAX_BLOC 16
const unsigned char hex_asc[] = "0123456789abcdef";
static void hexDump (char *desc, void *addr, int len, int bloc) {
    int i, lx, la, l, line;
	long offset = 0;
    unsigned char hexbuf[MAX_BLOC * 3 + 1];	// Hex part of the data (2 char + 1 space)
	unsigned char ascbuf[MAX_BLOC + 1];	// ASCII part of the data
    unsigned char *pc = (unsigned char*)addr;
	unsigned char ch;
	
	// nothing to output
	if (!len)
		return;

	// Limit the line length to MAX_BLOC
	if (bloc > MAX_BLOC) 
		bloc = MAX_BLOC;
		
	// Output description if given.
    if (desc != NULL)
		LOG("%s:\n", desc);
	
	line = 0;
	do
		{
		l = len - (line * bloc);
		if (l > bloc)
			l = bloc;
	
		for (i=0, lx = 0, la = 0; i < l; i++) {
			ch = pc[i];
			hexbuf[lx++] = hex_asc[((ch) & 0xF0) >> 4];
			hexbuf[lx++] = hex_asc[((ch) & 0xF)];
			hexbuf[lx++] = ' ';
		
			ascbuf[la++]  = (ch > 0x20 && ch < 0x7F) ? ch : '.';
			}
	
		for (; i < bloc; i++) {
			hexbuf[lx++] = ' ';
			hexbuf[lx++] = ' ';
			hexbuf[lx++] = ' ';
		}	
		// nul terminate both buffer
		hexbuf[lx++] = 0;
		ascbuf[la++] = 0;
	
		// output buffers
		LOG("%04x %s %s\n", line * bloc, hexbuf, ascbuf);
		
		line++;
		pc += bloc;
		}
	while (line * bloc < len);
}

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val) {
	char buff_topic[6];
	char buff_message[7];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%04d%", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%u", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
	}

static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	snprintf(buff_message, 12, "%f", val);
//	LOG("%s %s", buff_topic, buff_message);
	mosquitto_publish(_client, 0, buff_topic, strlen(buff_message), buff_message, 0, false);

	}

// Handing of Mosquitto messages
void callback(char* topic, uint8_t* payload, unsigned int length) {
	// handle message arrived
	LOG("Mosquitto Callback\n");
}


/* Connect to the network. */
static bool connect(struct mosquitto *m) {
	int res = mosquitto_connect(m, BROKER_HOSTNAME, BROKER_PORT, KEEPALIVE_SECONDS);
	LOG("Connect return %d\n", res);
	return res == MOSQ_ERR_SUCCESS;
}

/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
	if (res == 0) {   /* success */
		LOG("Connect succeed\n");
	} else {
		die("connection refused\n");
	}
}

/* Handle a message that just arrived via one of the subscriptions. */
static void on_message(struct mosquitto *m, void *udata, const struct mosquitto_message *msg) 
{
    char strPayload[100];
	if (msg == NULL) { return; }
	LOG("-- got message @ %s: (%d, QoS %d, %s) '%s'\n",
		msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
		msg->payload);
#ifdef DEBUG
    printf("MQTT-Topic:  %s: %s\n",msg->topic,msg->payload);
#endif


    int i;
    mes.nodeID=NODEID;      // gateway is node 1
    mes.fltVal=0;
    mes.intVal=0;
    mqttToSend = false;
    error = 4;


    if(strlen(msg->topic)==27)  // corect topic length?
    {
        dest = (msg->topic[19]-'0')*10 + msg->topic[20]-'0';  // extract target node ID from MQTT topic
        DID = (msg->topic[25]-'0')*10 + msg->topic[26]-'0';      // extract device ID from MQTT topic
#ifdef DEBUG
        printf("dest = %d, DID = %d\n", dest, DID);
#endif
    }
    //msg->payload[msg->payloadlen]='\0';     // terminate string with a NULL
    sprintf(strPayload,"%s",msg->payload);    // convert to String
    mes.devID = DID;
    mes.cmd = 0;    // default is 'SET' value
#ifdef DEBUG
    printf("strPayload = %s, %d\n", strPayload,strlen(strPayload));
#endif

    if(IsREAD(strPayload))  // in this case 'READ' value
    {
        mes.cmd = 1;
    }

    if(msg->payloadlen == 0)
    {
        error = 2;      // no payload sent
    }
    else
    {
        StatMess = ( DID==5 || DID==6 || DID==8 || (DID>=16 && DID<32));
        RealMess = (( DID==0 || DID==2 || DID==3 || DID==4 || (DID>=40 && DID<72))&& mes.cmd==1);
        IntMess = (DID==1 || DID==7 || DID==9 || (DID >=32 && DID <40));
        StrMess = (DID==72);
    }
   
   if(dest == 1 && DID == 0) 
    {          // gateway uptime wanted
        sprintf(buff_mess,  "%d", upTime);  
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev00"); // construct MQTT topic and message
        mosquitto_publish(m, 0, &buff_topic[0], strlen(buff_mess), buff_mess, 0, false);
        error =0;
    }
    
    if (dest == 1 && DID == 3) 
    {          // gateway version wanted
        for (i=0; i<sizeof(VERSION); i++)
        { 
            buff_mess[i] = (VERSION[i]); 
        }
        mes.payLoad[i] = '\0';
        sprintf(buff_topic, "home/rfm_gw/nb/node01/dev03"); // construct MQTT topic and message
        mosquitto_publish(m, 0, &buff_topic[0], strlen(buff_mess), buff_mess, 0, false);
        error =0;
    }
    
    if (dest>1 && StatMess) 
    {         // node status device
        mqttToSend = true; 
        /*
        if (strPayload == "ON") mes.intVal = 1;     // payload value is state 
        else  if (strPayload == "OFF") mes.intVal = 0;
        else if (strPayload != "READ") { mqttToSend = false; error = 3;}// invalid payload; do not process
            */
        if(IsON(strPayload))
            mes.intVal = 1; // payload value is state
        else if(IsOFF(strPayload))
            mes.intVal = 0;
        else if(IsREAD(strPayload))
        {
            mqttToSend = false;
            error = 3;  // invalid payload; do not process
        }
    }

    if (dest>1 && (DID >=40 && DID <48)) 
    {
        //if (strPayload == "READ") mqttToSend = true; 
        if(IsREAD(strPayload)) mqttToSend = true;
        else {mqttToSend = false; error = 3;}   // invalid payload; do not process
    }
    if (dest>1 && RealMess) 
    {         // node read device
        mqttToSend = true; 
    }
                                                                      
    if( dest>1 && IntMess ) 
    {          // node integer device
        if (mes.cmd == 0) 
        {
            int error_code;
            mes.intVal=strtoi(strPayload, &error_code);
#ifdef DEBUG
            printf("RealMess: dest=%d, %d, %s, error: %d\n", dest, mes.intVal, strPayload,error_code);
#endif

            if(error_code!=0)
            {
#ifdef DEBUG
                printf("Int conversion error_code:  %d\n", error_code);
#endif
                mqttToSend = false;
            } else {
                mqttToSend = true;
            }
            mqttToSend = true;
               
        //    mes.intVal = strPayload.toInt();  // timer/polling/Integer is in MQTT message
        //mqttToSend = true;
        }
    }

    if ( dest>1 && StrMess ) 
    {          // node string device
        if (mes.cmd == 0) 
        {
            sprintf(mes.payLoad,"%s",msg->payload);    // convert to String
            //int i; 
            //for (i=0; i<32; i++)
            //{ 
            //    mes.payLoad[i]=(char *)msg->payload[i]; 
            //}
        }
        mqttToSend = true;
    }
    if (mqttToSend && (error == 4)) 
        error = 0;    // valid device has been selected, hence error = 0
    respNeeded = mqttToSend;      // valid request needs radio response
#ifdef DEBUG
    printf("%s\n", strPayload);
    printf("dest:  %d, cmd:  %d intVal:  %d, fltVal: %.2f\n", dest, mes.cmd, mes.intVal, mes.fltVal);
#endif
/*		

			theStats.messageSent++;
			if (rfm69->sendWithRetry(data.nodeID,(const void*)(&data),sizeof(data))) {
				LOG("Message sent to node %d ACK", data.nodeID);
				theStats.ackReceived++;
				}
			else {
				LOG("Message sent to node %d NAK", data.nodeID);
				theStats.ackMissed++;
    */
}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
//	LOG(" -- published successfully\n");
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid,
		int qos_count, const int *granted_qos) {
//	LOG(" -- subscribed successfully\n");
}

/* Register the callbacks that the mosquitto connection will use. */
static bool set_callbacks(struct mosquitto *m) {
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_publish_callback_set(m, on_publish);
	mosquitto_subscribe_callback_set(m, on_subscribe);
	mosquitto_message_callback_set(m, on_message);
	return true;
}

static int strtoi(const char *nptr, int *error_code) 
{
    char *endptr;
    errno = 0;
    long i = strtol(nptr, &endptr, 10);
    #if LONG_MIN < INT_MIN || LONG_MAX > INT_MAX
    if (errno || i > INT_MAX || i < INT_MIN) 
    {
        *error_code = 1; // FAIL_INT_OVERFLOW;
        errno = ERANGE;
        i = i > 0 : INT_MAX : INT_MIN;
    }
    #else
    if (errno == ERANGE) 
    {
        *error_code = 2;    // FAIL_OVERFLOW;
    }
    #endif
    else if (endptr == nptr) 
    {
        *error_code = 3; // FAIL_NO_CONVERT;
    } else if (*endptr != '\0') {
    *error_code = 3;    //  FAIL_EXTRA_JUNK;
    }
   return (int) i;
}


static int mystrncmp(const char *s1, const char *s2, size_t n)
{
    for ( ; n > 0; s1++, s2++, --n)
        if (*s1 != *s2)
            return ((*(unsigned char *)s1 < *(unsigned char *)s2) ? -1 : +1);
        else if (*s1 == '\0')
            return 0;
    return 0;
}


static bool IsREAD(char *s)
{
#ifdef DEBUG
    printf("IsREAD:  %s, strlen: %d\n",s,strlen(s));
#endif

    if(s[0]=='R' && s[1]=='E' && s[2]=='A' && s[3]=='D' && strlen(s)==4)
        return true;
    return false;
}

static bool IsON(char *s)
{
    if(s[0]=='O' && s[1]=='N' && strlen(s)==2)
        return true;
    return false;
}

static bool IsOFF(char *s)
{
    if(s[0]=='O' && s[1]=='F' && s[2]=='F' && strlen(s)==3)
        return true;
    return false;
}

