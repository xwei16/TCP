/* 
 * File:   receiver_main.c
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

#include <iostream>
#include <queue>

#define MAX_PAYLOAD_SIZE 1024
#define MAX_QUEUE_SIZE 900
using namespace std;

enum data_t {NO_FIN, FIN, FINACK, TRASH};
typedef struct packet{  
    unsigned long long int sequence_num;   // 4-byte
    unsigned long long int ack_num;        // 4-byte
    int payload_size;   // 4-byte
    data_t data_type;
    char buffer[MAX_PAYLOAD_SIZE];  // 1024-byte, but depending on the actual buffer
    packet(){
        data_type = NO_FIN;
        sequence_num = 0;
        ack_num = 0;
        payload_size = 0;
        memset(buffer, 0, MAX_PAYLOAD_SIZE);
    }
}packet_t;

struct comparator {
    bool operator()(const packet_t& a, const packet_t& b) {
        return a.sequence_num > b.sequence_num;
    }
};

priority_queue<packet_t,vector<packet_t>, comparator> min_heap; 

struct sockaddr_in si_me, si_other;
int s, slen;
long long int ACK = -1;

void diep(char *s) {
    perror(s);
    exit(1);
}

// take the pointer of the packet and send to server
void send_packet(packet_t * packet_ptr){
    if(sendto(s, packet_ptr, sizeof(packet_t), 0, (struct sockaddr*)&si_other, sizeof(si_other)) < 0){ // check if error happened
        diep("send failed!");
    }
}

void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {
    
    slen = sizeof (si_other);


    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep("socket");

    memset((char *) &si_me, 0, sizeof (si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(myUDPport);
    si_me.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Now binding\n");
    if (bind(s, (struct sockaddr*) &si_me, sizeof (si_me)) == -1)
        diep("bind");


	/* Now receive data and send acknowledgements */  
    // FILE * 
    FILE * fp = fopen(destinationFile, "wb");
    if (fp == NULL) {
        printf("Could not open file to write.\n");
        exit(1);
    }

    packet_t * pkt = new packet();
    while(true){ 
        if(recvfrom(s, pkt, sizeof(packet_t), 0, (struct sockaddr*)&si_other, (socklen_t*)&slen) == -1){  
            cout << "recv Error" << endl;
            exit(1);
        }
        if(pkt->data_type == FIN){ // operation finished    
            pkt->data_type = FINACK;
            // send_packet(pkt);           
            break;   
        }
        // if(pkt->data_type == TRASH){
        //     if(pkt->sequence_num == ACK + 1){
        //         std::cout << "HIT" << endl;
        //         ACK++;        
        //     }
        //     // memset(pkt, 0, sizeof(struct packet));

        //     pkt->ack_num = ACK;
        //     send_packet(pkt);
        //     std::cout << "packet ACK: " << pkt->sequence_num << endl;
        //     continue;
        // }
        //write bytes to file
        //push packet to queue
        if (pkt->sequence_num == ACK + 1){
            if(pkt->data_type != TRASH){
                fwrite(pkt->buffer, 1, pkt->payload_size, fp);
            }
            ACK++;
            // memset(pkt, 0, sizeof(struct packet));
            // pkt->ack_num = ACK;
            // send_packet(pkt);        
        }
        else if (min_heap.size() > MAX_QUEUE_SIZE){
            //drop packet
            // cout << "PACKET DROPPED" << endl;
            continue;
        } 
        else if(((long long)(pkt->sequence_num)) <= ACK){
            memset(pkt, 0, sizeof(struct packet));
            pkt->ack_num = ACK;
            send_packet(pkt);
            continue;
        }
        else {
            //push
            min_heap.push(*pkt);
        }

        while(!min_heap.empty() && min_heap.top().sequence_num == ACK + 1){
            packet_t cur_pkt = min_heap.top();
            if(cur_pkt.data_type != TRASH){
                fwrite(cur_pkt.buffer, 1, cur_pkt.payload_size, fp);
            }
            ACK++;
            min_heap.pop();
        }
        
        //ACK
        memset(pkt, 0, sizeof(struct packet));
        pkt->ack_num = ACK;
        send_packet(pkt);
    }

    fclose(fp);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 40000;
    packet * fin_ack = new packet();
    fin_ack->data_type = FINACK;
    for(int i = 0; i < 3; ++i){
        send_packet(fin_ack);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeval));
        if(recvfrom(s, pkt, sizeof(packet_t), 0, (struct sockaddr*)&si_other, (socklen_t*)&slen) == -1){
            continue;
        }
        if(pkt->data_type == FINACK){
            break;
        }
    }

    delete fin_ack;
    delete pkt;


    close(s);
	printf("%s received.", destinationFile);
    return;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);

    reliablyReceive(udpPort, argv[2]);
}

