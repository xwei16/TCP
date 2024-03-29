/* 
 * File:   sender_main.c
 * Author: 
 *
 * Created on 
 */

// score 80 version
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>

#include <errno.h>
#include <iostream>
#include <vector>
#include <string>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <math.h>


#define MAX_PAYLOAD_SIZE 1024
#define ZERO_PUSH_SIZE 200
#define CW_SIZE 75
#define SST_SIZE 300
#define OUTGOING_SIZE 512


using namespace std;

// event type
enum event_t {NEW_ACK, DUP_ACK, HANDSHAKE, TIMEOUT, DUP_ACK_EQ_3, NO_OP};

// state type
enum state_t {SLOW_START, FAST_RECOVERY, CONGESTION_AVOIDANCE};
enum data_t {NO_FIN, FIN, FINACK, TRASH};
// self-defined pack struct
typedef struct packet{  
    unsigned long long sequence_num;   // 8-byte
    unsigned long long ack_num;        // 8-byte
    int payload_size;   // 4-byte
    data_t data_type;
    char buffer[MAX_PAYLOAD_SIZE];  // 1024-byte, but depending on the actual buffer
    packet(){
        sequence_num = 0;
        ack_num = 0;
        payload_size = 0;
        data_type = NO_FIN;
        memset(buffer, 0, MAX_PAYLOAD_SIZE);
    }
}packet_t;

unordered_set<unsigned long long int> ack_map; // use to keep track of dupACK_EQ_3, dupACK and new ACK

queue <packet_t*> outgoing; // the packet to be sent, if packet is send, it will enqueue to window
queue <packet_t*> window; //representing a sliding window, which size is defined by CW - change over time, store packets that haven't been ack

struct sockaddr_in si_other;
int s, slen;
float SST = SST_SIZE;
int DUPACK = 0;
float CW = CW_SIZE;
state_t current_state = SLOW_START;
unsigned long long zero_bytes_counter = 0;
unsigned long long seq_num_starter = 0;
// unsigned long long received_ack = 0; // commnent this out if needed

void state_trans(packet_t *, event_t);
void slowStart(packet_t* pkt, event_t event);
void congestionAvoidance(packet_t* pkt, event_t event);
void fastRecovery(packet_t* pkt, event_t event);
void tx_new_packets();
void send_packet(packet_t * packet_ptr);
void timeout_retransmit();
void dup_ack_eq_3_handler();
void zero_bytes_en_outgoing_queue(unsigned long long int num_packet_to_send);

void diep(char *s) {
    perror(s);
    exit(1);
}

// take the pointer of the packet and send to server
void send_packet(packet_t * packet_ptr){
    
    // if(packet_ptr){
    //     std::cout << packet_ptr->sequence_num << endl;
    // }
    if(sendto(s, packet_ptr, sizeof(packet_t), 0, (struct sockaddr*)&si_other, sizeof(si_other)) < 0){ // check if error happened
        diep("send failed!");
    }
}

// handling new acknowledgement
void new_ack_fun(unsigned long long int cur_ack_num) {
    //remove acked packet from window
    packet_t * packet = window.front();
    while (!window.empty() && packet->sequence_num <= cur_ack_num) {
        ack_map.erase(packet->ack_num); // remove from ack_map, since packet is acknowledged
        window.pop();                   // remove from window
        delete packet;
        packet = window.front();
    }
    // std::cout << packet->sequence_num << endl;
    //send CW-window.size() # of packet from outgoing queue, and put them in window
    tx_new_packets();
    // int counter = (int)CW - window.size();
    // while (counter-- > 0 && !outgoing.empty()) {
    //     packet_t * packet_to_send = outgoing.front();
    //     send_packet(packet_to_send);
    //     outgoing.pop();
    //     window.push(packet_to_send);
    // } 
}

// handling timeout transition and resending window based packket
void timeout_retransmit() {
    //resend cw-base packet
    int window_size = window.size();
    for(int i = 0; i < window_size; ++i){
        packet_t * win_base_pkt = window.front();   // getting window base packet
        send_packet(win_base_pkt);                  // sending cw-base packet
        window.pop();
        window.push(win_base_pkt);
    }
    current_state = SLOW_START;   
              // all time_out goes back to slow start
}

void dup_ack_eq_3_handler() {
    //resend cw-base packet
    packet_t * win_base_pkt = window.front();   // getting window base packet
    send_packet(win_base_pkt);                  // sending cw-base packet
    current_state = FAST_RECOVERY;                 // all time_out goes back to slow start

    tx_new_packets();
    // int counter = (int)CW - window.size();
    // while (counter-- > 0 && !outgoing.empty()) {
    //     packet_t * packet_to_send = outgoing.front();
    //     send_packet(packet_to_send);
    //     outgoing.pop();
    //     window.push(packet_to_send);
    // }
}

//send CW-window.size() # of packet from outgoing queue, and put them in window
void tx_new_packets(){
    int counter = (int)CW - window.size();
    // std::cout << "# of Pkt to be send: " << counter << endl;
    // std::cout << "OOPS: " << counter << endl;
    while (counter-- > 0 && !outgoing.empty()) {
        packet_t * packet_to_send = outgoing.front();
        // std::cout << "the pkt: " << packet_to_send -> sequence_num << endl;
        send_packet(packet_to_send);
        outgoing.pop();
        window.push(packet_to_send);
    }
}

// controlling current_state and next_state
void state_trans(packet_t* pkt, event_t event){

    switch (current_state){
        case SLOW_START: 
            slowStart(pkt, event);
            break;
        case CONGESTION_AVOIDANCE:
            congestionAvoidance(pkt, event);
            break;
        case FAST_RECOVERY:
            fastRecovery(pkt, event);
            break;
        default:
            break;
    }
}

// slow start state actions
void slowStart(packet_t* pkt, event_t event) {
    // event = NoOp -> need check
    // event== timeout 
    // event = HANDSHAKE; // default event
    //check event
    if (event == NO_OP) {
        unsigned long long int cur_ack_num = pkt->ack_num;
        if (ack_map.count(cur_ack_num) == 0) {
            ack_map.insert(cur_ack_num);
            event = NEW_ACK;
        } else {
            event = DUP_ACK;
            if (DUPACK>=2) {
                event = DUP_ACK_EQ_3;
            }
        }
    }
    
    // std::cout << "current state: " << current_state << " event: " << event << endl;
    switch (event){

        case NEW_ACK:
            CW = CW + 1;
            // CW += ((received_ack > 0) ? (double)(pkt->ack_num - received_ack) : 1); // a new attempt comment out if needed
            // received_ack = pkt->ack_num; // a new attempt comment out if needed
            DUPACK = 0;
            //send packet based on CW
            new_ack_fun(pkt->ack_num);
            if(CW >= SST){
                current_state = CONGESTION_AVOIDANCE;
            }
            else{
                current_state = SLOW_START;
            }
            // current_state = (CW >= SST) ? CONGESTION_AVOIDANCE : current_state;
            break;
        case DUP_ACK:
            DUPACK++;
            break;
        case TIMEOUT: // timeout self looping
            SST = CW / 2.0;
            CW =CW_SIZE;
            DUPACK = 0;
            timeout_retransmit();
            // retransmit CW_base packet
            break;
        case DUP_ACK_EQ_3:
            SST = CW / 2.0;
            CW = SST + 3;
            
            // DUPACK = 0; //reset dupACK
            dup_ack_eq_3_handler(); // may return immediately
            return; 
        default:
            break;
    }

    //state trans check
    // if (event!=DUP_ACK_EQ_3)
}

// congestion avoidance state action
void congestionAvoidance(packet_t* pkt, event_t event){

    if (event == NO_OP) {
        unsigned long long int cur_ack_num = pkt->ack_num;
        if (ack_map.count(cur_ack_num) == 0) {
            ack_map.insert(cur_ack_num);
            event = NEW_ACK;
        } else {
            event = DUP_ACK;
            if (DUPACK>=2) {
                event = DUP_ACK_EQ_3;
            }
        }
    }
    
    switch (event){
        case NEW_ACK:   
            // while(received_ack++ < pkt->ack_num) {
                CW = CW + (1.0/CW);   // the formula
            // }
            // received_ack = pkt->ack_num; // a new attempt, comment out if needed
            DUPACK = 0;
            new_ack_fun(pkt->ack_num);
            break;
        case DUP_ACK:
            DUPACK++;
            break;
        case TIMEOUT:
            SST = CW / 2.0;
            CW = CW_SIZE;
            DUPACK = 0;
            timeout_retransmit();
            break;
        case DUP_ACK_EQ_3:
            SST = CW / 2.0;
            CW = SST + 3;
            //reset dupACK
            // DUPACK = 0;
            // may return immediately
            dup_ack_eq_3_handler();
            break;
        default:
            break;
    }
}

// fast recovery state actions
void fastRecovery(packet_t* pkt, event_t event){

    if (event == NO_OP) {
        unsigned long long int cur_ack_num = pkt->ack_num;
        if (ack_map.count(cur_ack_num) == 0) {
            ack_map.insert(cur_ack_num);
            event = NEW_ACK;
        } else {
            event = DUP_ACK;
        }
    }

    switch (event){
        case DUP_ACK:
            // DUPACK++;
            CW = CW + 1;
            tx_new_packets();
            break;
        case TIMEOUT:
            SST = CW / 2.0;
            CW = CW_SIZE;
            DUPACK = 0;
            timeout_retransmit();
            break;
        case NEW_ACK:
            DUPACK = 0;
            CW = SST;
            // received_ack = pkt->ack_num; // new try, comment out if needed
            new_ack_fun(pkt->ack_num);
            current_state = CONGESTION_AVOIDANCE;
            break;
        default:
            break;
    }
}

void prepare_outgoing_q(FILE *fp, unsigned long long int &bytes_to_send){
    // get num of packet to send
    unsigned long long bytes_to_read;
    
    // prepare packet and push to outgoing queue
    unsigned long long  i = seq_num_starter;
    while(bytes_to_send > 0 && outgoing.size() < OUTGOING_SIZE){
        bytes_to_read = min(bytes_to_send, (unsigned long long)(MAX_PAYLOAD_SIZE));
        packet_t * packet = new packet_t();
        packet->sequence_num = i;

        int read_bytes = fread(packet->buffer, 1, bytes_to_read, fp);
        if(read_bytes == 0){
            delete packet;
            break;
        }
        packet->payload_size = read_bytes;
        outgoing.push(packet); // push packets to the queue
        bytes_to_send -= read_bytes;
        // update the sequence number
        i += 1;
    }
    zero_bytes_counter = bytes_to_send;
    seq_num_starter = i;
    
    // std::cout << zero_bytes_counter << endl;
    // std::cout << outgoing.size() << endl;
    // std::cout << window.size() << endl;

}


void zero_bytes_en_outgoing_queue(unsigned long long int num_packet_to_send){
    // get num of packet to send
    // unsigned long long bytes_to_read;
    
    // prepare packet and push to outgoing queue
    unsigned long long  i = 0;
    while(i<num_packet_to_send){
        packet_t * packet = new packet_t();
        packet->sequence_num = (i+seq_num_starter); // orginal is i
        packet->data_type = TRASH;
        outgoing.push(packet); // push packets to the queue
        i += 1;
    }
    seq_num_starter += num_packet_to_send;
}


/**
 * hostname: destination
 * hostUDPport: port number
 * filename: file that the client request
 * bytesToTransfer: size of the file
*/
void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer) {
    //Open the file
    FILE *fp;
    fp = fopen(filename, "rb");
    printf("%s\n", filename);
    if (fp == NULL) {
        printf("Could not open file to send.\n");
        exit(1);
    }

	/* Determine how many bytes to transfer */

    slen = sizeof (si_other); 

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)   // grab socket to s
        diep("socket");

    memset((char *) &si_other, 0, sizeof (si_other));   // clear si_other
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(hostUDPport);
    if (inet_aton(hostname, &si_other.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }

	/* Send data and receive acknowledgements on s*/
    
    //set timeout
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 40000;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1){
        diep("setsockopt failed");
    }

    //prepare outgoing queue
    prepare_outgoing_q(fp, bytesToTransfer);

    packet_t* pkt = new packet_t();
    //send first one
    tx_new_packets();
    // std::cout << outgoing.size() << endl;
    // std::cout << window.size() << endl;
    while(true) {
        prepare_outgoing_q(fp, bytesToTransfer);
        if (outgoing.empty() && window.empty()) {
            break;
        }
        
        // break condition
        if(recvfrom(s, pkt, sizeof(packet_t), 0, (struct sockaddr*)&si_other, (socklen_t*)&slen) == -1){  
            if(errno != EAGAIN || errno != EWOULDBLOCK){
                std::cout << "recv Complaint" << endl;
                exit(1);
            } 
            if (!window.empty()){
                state_trans(pkt, TIMEOUT);
            }
        } else {
            // state and state transition
            state_trans(pkt, NO_OP);
        }
        // std::cout << "sending" << endl;
        // std::cout << "remaining: " << bytesToTransfer << endl;
        // std::cout << "outgo: " << outgoing.size() << endl;
        // std::cout << "window: " << window.size() << endl;
        // std::cout << "CW: " << CW << endl;
        // std::cout << "SST: " << SST << endl;
        CW = (CW < 1) ? 1 : CW;
        SST = (SST < 1) ? 1 : SST;
    }


    //0 packets for friendliness 
    // print
    // std::cout << zero_bytes_counter << endl;
    // long long int count = 0;
    // SST = (SST < 64) ? SST : 64;
    unsigned long long int zero_push_s = ZERO_PUSH_SIZE;
    if(zero_bytes_counter > 0){
        zero_bytes_en_outgoing_queue(zero_push_s);
        zero_bytes_counter -= min (zero_bytes_counter, zero_push_s*(unsigned long long)MAX_PAYLOAD_SIZE);
        tx_new_packets();
    }
    
    while(true) {
        if(zero_bytes_counter <= 0 && window.empty() && outgoing.empty()){
            break;
        }
        if (outgoing.empty() && zero_bytes_counter > 0) {
            zero_bytes_en_outgoing_queue(zero_push_s);
            zero_bytes_counter -= min (zero_bytes_counter, zero_push_s*(unsigned long long)MAX_PAYLOAD_SIZE);
        }
        if(recvfrom(s, pkt, sizeof(packet_t), 0, (struct sockaddr*)&si_other, (socklen_t*)&slen) == -1){  
            if(errno != EAGAIN || errno != EWOULDBLOCK){
                std::cout << "recv Complaint" << endl;
                exit(1);
            } 
            if (!window.empty()){
                // std::cout << pkt->sequence_num << endl;
                state_trans(pkt, TIMEOUT);
            }
        } else {
            // state and state transition
            state_trans(pkt, NO_OP);
        }
        // std::cout << "sending" << endl;

        // std::cout << "zero outgo: " << outgoing.size() << endl;
        // std::cout << "zero window: " << window.size() << endl;
        // std::cout << "zero CW: " << CW << endl;
        // std::cout << "zero SST: " << SST << endl;
        // std::cout << "zero byte count: " << zero_bytes_counter << endl;

        CW = (CW < 1) ? 1 : CW;
        SST = (SST < 1) ? 1 : SST;
        // count += 1;
        // if(window.empty()){
        //     break;
        // }
    }

    // send FIN
    packet_t *fin_pkt = new packet_t();
    fin_pkt->data_type=FIN;
    send_packet(fin_pkt);
    while(true) {
        // break condition
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeval));
        if(recvfrom(s, pkt, sizeof(packet_t), 0, (struct sockaddr*)&si_other, (socklen_t*)&slen) == -1){  
            if(errno != EAGAIN || errno != EWOULDBLOCK){
                std::cout << "recv Complaint" << endl;
                exit(1);
            } else{
                //timeout - resend
                send_packet(fin_pkt);
            }
        } else {
            // received finack
            if (pkt->data_type==FINACK) {
                std::cout <<"recv FINACK - DONE" << endl;
                fin_pkt->data_type = FINACK;
                send_packet(fin_pkt);
                break;
            }
        }
    }


    
    delete pkt;
    delete fin_pkt;

    printf("Closing the socket\n");
    close(s);
    return;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;
    unsigned long long int numBytes;

    if (argc != 5) { 
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    udpPort = (unsigned short int) atoi(argv[2]);
    numBytes = atoll(argv[4]);



    reliablyTransfer(argv[1], udpPort, argv[3], numBytes);


    return (EXIT_SUCCESS);
}

