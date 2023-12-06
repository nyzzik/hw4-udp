#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/un.h>
#include <time.h>

#include "du-proto.h"

static char _dpBuffer[DP_MAX_DGRAM_SZ];
static int  _debugMode = 1;

/*
Creates DPSession structure and reserves memory. Sets attributes of the structure. Returns the created structure.

*/

static dp_connp dpinit(){
    dp_connp dpsession = malloc(sizeof(dp_connection));
    bzero(dpsession, sizeof(dp_connection));
    dpsession->outSockAddr.isAddrInit = false;
    dpsession->inSockAddr.isAddrInit = false;
    dpsession->outSockAddr.len = sizeof(struct sockaddr_in);
    dpsession->inSockAddr.len = sizeof(struct sockaddr_in);
    dpsession->seqNum = 0;
    dpsession->isConnected = false;
    dpsession->dbgMode = true;
    return dpsession;
}
/*
    Closes the DPsession and free the memory allocated for it.
*/
void dpclose(dp_connp dpsession){
    free(dpsession);
}

/*
    Returns the maximum buffer size for the DP
*/
int  dpmaxdgram(){
    return DP_MAX_BUFF_SZ;
}

/*
    Initializes the DP and returns NULL if it failed. Creates the socket fd. Sets server information. Sets socket options. Returns the DP Connection structure.
*/
dp_connp dpServerInit(int port){
    struct sockaddr_in* servaddr;
    int* sock;
    int rc;

    dp_connp dpc = dpinit();
    if(dpc == NULL) {
        perror("drexel protocol create failure");
        return NULL;
    }

    sock = &(dpc->udp_sock);
    servaddr = &(dpc->inSockAddr.addr);


    // Creating socket file descriptor 
    if((*sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        return NULL;
    }

    // Filling server information 
    servaddr->sin_family = AF_INET; // IPv4 
    servaddr->sin_addr.s_addr = INADDR_ANY;
    servaddr->sin_port = htons(port);

    // Set socket options so that we dont have to wait for ports held by OS
    if(setsockopt(*sock, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int)) < 0){
        perror("setsockopt(SO_REUSEADDR) failed");
        close(*sock);
        return NULL;
    }
    if(setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0){
        perror("setsockopt(SO_REUSEADDR) failed");
        close(*sock);
        return NULL;
    }
    if((rc = bind(*sock, (const struct sockaddr*)servaddr,
        dpc->inSockAddr.len)) < 0)
    {
        perror("bind failed");
        close(*sock);
        return NULL;
    }

    dpc->inSockAddr.isAddrInit = true;
    dpc->outSockAddr.len = sizeof(struct sockaddr_in);
    return dpc;
}

/*
    Initializes the DP client. Creates the socket FD. Fills in the server information and sets the inbound address to the same thing as the outbound address and returns the DP client structure.
*/
dp_connp dpClientInit(char* addr, int port){
    struct sockaddr_in* servaddr;
    int* sock;

    dp_connp dpc = dpinit();
    if(dpc == NULL) {
        perror("drexel protocol create failure");
        return NULL;
    }

    sock = &(dpc->udp_sock);
    servaddr = &(dpc->outSockAddr.addr);

    // Creating socket file descriptor 
    if((*sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        return NULL;
    }

    // Filling server information 
    servaddr->sin_family = AF_INET;
    servaddr->sin_port = htons(port);
    servaddr->sin_addr.s_addr = inet_addr(addr);
    dpc->outSockAddr.len = sizeof(struct sockaddr_in);
    dpc->outSockAddr.isAddrInit = true;

    // The inbound address is the same as the outbound address
    memcpy(&dpc->inSockAddr, &dpc->outSockAddr, sizeof(dpc->outSockAddr));

    return dpc;
}

/*
    Retrieves receive length using the dp struct. Returns if the length DP_CONNECTION_CLOSED if the connection was closed. If the received length is greater than the size of the pdu, it copies the dp buffer into the buff variable. Returns the dgram size.
*/
int dprecv(dp_connp dp, void* buff, int buff_sz){

    dp_pdu* inPdu;
    int rcvLen = dprecvdgram(dp, _dpBuffer, sizeof(_dpBuffer));

    if(rcvLen == DP_CONNECTION_CLOSED)
        return DP_CONNECTION_CLOSED;

    inPdu = (dp_pdu*)_dpBuffer;
    if(rcvLen > sizeof(dp_pdu))
        memcpy(buff, (_dpBuffer + sizeof(dp_pdu)), inPdu->dgram_sz);

    return inPdu->dgram_sz;
}

/*
    Returns if the buffer is greater than the max size. Receives the raw bytes. Sets the error code if the number of bytes in are less than the size of the pdu. Copies the buffer into a pdu struct. Checks if there is no error code and then increments the sequence number. Creates the outbound pdu. Checks if there is an error code and sends the outpdu and returns if the actual size sent does not equal the pdu size. Checks the inbound pdu mtype and sets the outpdu mtype accordingly and sends the outpdu. Returns an error if the actual send size is less thant he pdu size. If the mtype is DP_MT_CLOSE it also closes the DP. Function returns the bytes in.
*/
static int dprecvdgram(dp_connp dp, void* buff, int buff_sz){
    int bytesIn = 0;
    int errCode = DP_NO_ERROR;

    if(buff_sz > DP_MAX_DGRAM_SZ)
        return DP_BUFF_OVERSIZED;

    bytesIn = dprecvraw(dp, buff, buff_sz);

    //check for some sort of error and just return it
    if(bytesIn < sizeof(dp_pdu))
        errCode = DP_ERROR_BAD_DGRAM;

    dp_pdu inPdu;
    memcpy(&inPdu, buff, sizeof(dp_pdu));
    if(inPdu.dgram_sz > buff_sz)
        errCode = DP_BUFF_UNDERSIZED;

    //Copy buffer back
    // memcpy(buff, (_dpBuffer+sizeof(dp_pdu)), inPdu.dgram_sz);


    //UDPATE SEQ NUMBER AND PREPARE ACK
    if(errCode == DP_NO_ERROR){
        if(inPdu.dgram_sz == 0)
            //Update Seq Number to just ack a control message - just got PDU
            dp->seqNum++;
        else
            //Update Seq Number to increas by the inbound PDU dgram_sz
            dp->seqNum += inPdu.dgram_sz;
    }
    else {
        //Update seq number to ACK error
        dp->seqNum++;
    }

    dp_pdu outPdu;
    outPdu.proto_ver = DP_PROTO_VER_1;
    outPdu.dgram_sz = 0;
    outPdu.seqnum = dp->seqNum;
    outPdu.err_num = errCode;

    int actSndSz = 0;
    //HANDLE ERROR SITUATION
    if(errCode != DP_NO_ERROR) {
        outPdu.mtype = DP_MT_ERROR;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if(actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
    }


    switch(inPdu.mtype){
    case DP_MT_SND:
        outPdu.mtype = DP_MT_SNDACK;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if(actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
        break;
    case DP_MT_CLOSE:
        outPdu.mtype = DP_MT_CLOSEACK;
        actSndSz = dpsendraw(dp, &outPdu, sizeof(dp_pdu));
        if(actSndSz != sizeof(dp_pdu))
            return DP_ERROR_PROTOCOL;
        dpclose(dp);
        return DP_CONNECTION_CLOSED;
    default:
    {
        printf("ERROR: Unexpected or bad mtype in header %d\n", inPdu.mtype);
        return DP_ERROR_PROTOCOL;
    }
    }

    return bytesIn;
}

/*
    Throws an error if the address isnt initialized. Receives bytes from the dp udp socket. Throws an error if the bytes received are less than 0. Sets the outSockAddr.isAddrInit to true. Checks if the bytes are greater than the size of the pdu and does nothing. Creates an inPdu from the buffer and prints it. Returns the bytes received.
*/
static int dprecvraw(dp_connp dp, void* buff, int buff_sz){
    int bytes = 0;

    if(!dp->inSockAddr.isAddrInit) {
        perror("dprecv: dp connection not setup properly - cli struct not init");
        return -1;
    }

    bytes = recvfrom(dp->udp_sock, (char*)buff, buff_sz,
        MSG_WAITALL, (struct sockaddr*)&(dp->outSockAddr.addr),
        &(dp->outSockAddr.len));

    if(bytes < 0) {
        perror("dprecv: received error from recvfrom()");
        return -1;
    }
    dp->outSockAddr.isAddrInit = true;

    //some helper code if you want to do debugging
    if(bytes > sizeof(dp_pdu)){
        if(false) {                         //just diabling for now
            dp_pdu* inPdu = buff;
            char* payload = (char*)buff + sizeof(dp_pdu);
            printf("DATA : %.*s\n", inPdu->dgram_sz, payload);
        }
    }

    dp_pdu* inPdu = buff;
    print_in_pdu(inPdu);

    //return the number of bytes received 
    return bytes;
}

/*
    Returns DP_BUFF_UNDERSIZED if the send buffer is greater than the max. Sends the dgram and returns the sent size.
*/
int dpsend(dp_connp dp, void* sbuff, int sbuff_sz){


    //For now, we will not be able to send larger than the biggest datagram
    if(sbuff_sz > dpmaxdgram()) {
        return DP_BUFF_UNDERSIZED;
    }

    int sndSz = dpsenddgram(dp, sbuff, sbuff_sz);

    return sndSz;
}

/*
    Throws an error if the outSockAddr isnt initialized. Returns DP_ERROR_GENERAL if the send buffer size is grater than the MAX buff size. Build the outPdu . Sets builds the DP Buffer using the send buffer. Calculates the total send size and sends the data. Prints a warning if the sent byte size is not equal to the total send size. Increments the outpdu sequence number. Return the amount of bytes sent minus the size of the dp pdu.
*/
static int dpsenddgram(dp_connp dp, void* sbuff, int sbuff_sz){
    int bytesOut = 0;

    if(!dp->outSockAddr.isAddrInit) {
        perror("dpsend:dp connection not setup properly");
        return DP_ERROR_GENERAL;
    }

    if(sbuff_sz > DP_MAX_BUFF_SZ)
        return DP_ERROR_GENERAL;

    //Build the PDU and out buffer
    dp_pdu* outPdu = (dp_pdu*)_dpBuffer;
    int    sndSz = sbuff_sz;
    outPdu->proto_ver = DP_PROTO_VER_1;
    outPdu->mtype = DP_MT_SND;
    outPdu->dgram_sz = sndSz;
    outPdu->seqnum = dp->seqNum;

    memcpy((_dpBuffer + sizeof(dp_pdu)), sbuff, sndSz);

    int totalSendSz = outPdu->dgram_sz + sizeof(dp_pdu);
    bytesOut = dpsendraw(dp, _dpBuffer, totalSendSz);

    if(bytesOut != totalSendSz){
        printf("Warning send %d, but expected %d!\n", bytesOut, totalSendSz);
    }

    //update seq number after send
    if(outPdu->dgram_sz == 0)
        dp->seqNum++;
    else
        dp->seqNum += outPdu->dgram_sz;

    //need to get an ack
    dp_pdu inPdu = { 0 };
    int bytesIn = dprecvraw(dp, &inPdu, sizeof(dp_pdu));
    if((bytesIn < sizeof(dp_pdu)) && (inPdu.mtype != DP_MT_SNDACK)){
        printf("Expected SND/ACK but got a different mtype %d\n", inPdu.mtype);
    }

    return bytesOut - sizeof(dp_pdu);
}


/*
    Throws an erro fi the outSockAddr is not initialized. Creates the outPdu using the send buffer. Sends the bytes using the udp socket and and prints the outPdu. Returns the amount of bytes sent.
*/
static int dpsendraw(dp_connp dp, void* sbuff, int sbuff_sz){
    int bytesOut = 0;
    // dp_pdu *pdu = sbuff;

    if(!dp->outSockAddr.isAddrInit) {
        perror("dpsendraw:dp connection not setup properly");
        return -1;
    }

    dp_pdu* outPdu = sbuff;
    bytesOut = sendto(dp->udp_sock, (const char*)sbuff, sbuff_sz,
        0, (const struct sockaddr*)&(dp->outSockAddr.addr),
        dp->outSockAddr.len);


    print_out_pdu(outPdu);

    return bytesOut;
}

/*
    Throws an error if the inSockAddr is not initialized. Creates a pdu struct. Receives raw data from the dp. Throws an error if the size of the data received is not equal to the size of the pdu. SEts the pdu mtype. Increments the sequence number. Calls dpsendraw. Throws an error if the size of the data sent does not equal the pdu. Sets the dp isConnected to true. returns True.
*/
int dplisten(dp_connp dp){
    int sndSz, rcvSz;

    if(!dp->inSockAddr.isAddrInit) {
        perror("dplisten:dp connection not setup properly - cli struct not init");
        return DP_ERROR_GENERAL;
    }

    dp_pdu pdu = { 0 };

    printf("Waiting for a connection...\n");
    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if(rcvSz != sizeof(pdu)) {
        perror("dplisten:The wrong number of bytes were received");
        return DP_ERROR_GENERAL;
    }

    pdu.mtype = DP_MT_CNTACK;
    dp->seqNum = pdu.seqnum + 1;
    pdu.seqnum = dp->seqNum;

    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));

    if(sndSz != sizeof(pdu)) {
        perror("dplisten:The wrong number of bytes were sent");
        return DP_ERROR_GENERAL;
    }
    dp->isConnected = true;
    //For non data transmissions, ACK of just control data increase seq # by one
    printf("Connection established OK!\n");

    return true;
}

/*
    Throws an error if the outSockAddr is not initialized. Builds a dp pdu struct. Sends the pdu and throws an error if the size sent does not equal the size of the pdu. Receives raw and throws an error if the size sent does not equal the size of the dp pdu. Throws an error if the pdu size does not equal DP_MT_CNTACK. Increments the sequence number and sets dp isConnected to true. Returns true.
*/
int dpconnect(dp_connp dp){

    int sndSz, rcvSz;

    if(!dp->outSockAddr.isAddrInit) {
        perror("dpconnect:dp connection not setup properly - svr struct not init");
        return DP_ERROR_GENERAL;
    }

    dp_pdu pdu = { 0 };
    pdu.mtype = DP_MT_CONNECT;
    pdu.seqnum = dp->seqNum;
    pdu.dgram_sz = 0;

    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));
    if(sndSz != sizeof(dp_pdu)) {
        perror("dpconnect:Wrong about of connection data sent");
        return -1;
    }

    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if(rcvSz != sizeof(dp_pdu)) {
        perror("dpconnect:Wrong about of connection data received");
        return -1;
    }
    if(pdu.mtype != DP_MT_CNTACK) {
        perror("dpconnect:Expected CNTACT Message but didnt get it");
        return -1;
    }

    //For non data transmissions, ACK of just control data increase seq # by one
    dp->seqNum++;
    dp->isConnected = true;
    printf("Connection established OK!\n");

    return true;
}

/*
    Builds a pdu. Calls dpsendraw and throws an error if the send size does not equal the size of the pdu. Calls dprecvraw and throws an error if the received size does not equalk the dp pdu size. Throws an error if the pdu mtype does not equal DP_MT_CLOSEACK. Closes the dp. Returns DP_CONNECTION_CLOSED.
*/
int dpdisconnect(dp_connp dp){

    int sndSz, rcvSz;

    dp_pdu pdu = { 0 };
    pdu.proto_ver = DP_PROTO_VER_1;
    pdu.mtype = DP_MT_CLOSE;
    pdu.seqnum = dp->seqNum;
    pdu.dgram_sz = 0;

    sndSz = dpsendraw(dp, &pdu, sizeof(pdu));
    if(sndSz != sizeof(dp_pdu)) {
        perror("dpdisconnect:Wrong about of connection data sent");
        return DP_ERROR_GENERAL;
    }

    rcvSz = dprecvraw(dp, &pdu, sizeof(pdu));
    if(rcvSz != sizeof(dp_pdu)) {
        perror("dpdisconnect:Wrong about of connection data received");
        return DP_ERROR_GENERAL;
    }
    if(pdu.mtype != DP_MT_CLOSEACK) {
        perror("dpdisconnect:Expected CNTACT Message but didnt get it");
        return DP_ERROR_GENERAL;
    }
    //For non data transmissions, ACK of just control data increase seq # by one
    dpclose(dp);

    return DP_CONNECTION_CLOSED;
}

/*
    Throws an error if the buffer size is less than the size of a dp pdu. Creates an empty buffer and copies the pdu to it. Returns the address after the pdu.
*/
void* dp_prepare_send(dp_pdu* pdu_ptr, void* buff, int buff_sz){
    if(buff_sz < sizeof(dp_pdu)) {
        perror("Expected CNTACT Message but didnt get it");
        return NULL;
    }
    bzero(buff, buff_sz);
    memcpy(buff, pdu_ptr, sizeof(dp_pdu));

    return buff + sizeof(dp_pdu);
}


//// MISC HELPERS
/*
    Prints the pdu details.
*/
void print_out_pdu(dp_pdu* pdu){
    if(_debugMode != 1)
        return;
    printf("PDU DETAILS ===>  [OUT]\n");
    print_pdu_details(pdu);
}
/*
    Prints the pdu details.
*/
void print_in_pdu(dp_pdu* pdu){
    if(_debugMode != 1)
        return;
    printf("===> PDU DETAILS  [IN]\n");
    print_pdu_details(pdu);
}
/*
    Prints the pdu details.
*/
static void print_pdu_details(dp_pdu* pdu){

    printf("\tVersion:  %d\n", pdu->proto_ver);
    printf("\tMsg Type: %s\n", pdu_msg_to_string(pdu));
    printf("\tMsg Size: %d\n", pdu->dgram_sz);
    printf("\tSeq Numb: %d\n", pdu->seqnum);
    printf("\n");
}

/*
    Converts the PDU mtype to string.
*/
static char* pdu_msg_to_string(dp_pdu* pdu){
    switch(pdu->mtype){
    case DP_MT_ACK:
        return "ACK";
    case DP_MT_SND:
        return "SEND";
    case DP_MT_CONNECT:
        return "CONNECT";
    case DP_MT_CLOSE:
        return "CLOSE";
    case DP_MT_NACK:
        return "NACK";
    case DP_MT_SNDACK:
        return "SEND/ACK";
    case DP_MT_CNTACK:
        return "CONNECT/ACK";
    case DP_MT_CLOSEACK:
        return "CLOSE/ACK";
    default:
        return "***UNKNOWN***";
    }
}

/*
 *  This is a helper for testing if you want to inject random errors from
 *  time to time. It take a threshold number as a paramter and behaves as
 *  follows:
 *      if threshold is < 1 it always returns FALSE or zero
 *      if threshold is > 99 it always returns TRUE or 1
 *      if (1 <= threshold <= 99) it generates a random number between
 *          1..100 and if the random number is less than the threshold
 *          it returns TRUE, else it returns false
 *
 *  Example: dprand(50) is a coin flip
 *              dprand(25) will return true 25% of the time
 *              dprand(99) will return true 99% of the time
 */
int dprand(int threshold){

    if(threshold < 1)
        return 0;
    if(threshold > 99)
        return 1;
    //initialize randome number seed
    srand(time(0));

    int rndInRange = (rand() % (100 - 1 + 1)) + 1;
    if(threshold < rndInRange)
        return 1;
    else
        return 0;
}
