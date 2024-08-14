// Little program to read tcpdump output file and extract packets with
// dclab headers. Snarf at least 64 bytes for this to work.
//
// TODO: we need to know whether packet is incoming or outgoing, so we
//  need to know which machine this trace was taken on.
//
// Copyright 2021 Richard L. Sites
//
// Compile with g++ -O2 pcaptojson.cc dclab_log.cc dclab_rpc.cc -lpcap -o pcaptojson


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <net/ethernet.h>
#include <netinet/tcp.h>
#include <pcap/pcap.h>

#include "basetypes.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"

#define PCAP_BUF_SIZE	1024
#define PCAP_SRC_FILE	2

void dumpbytes(const u_char* d,  int len) {
  for (int i = 0; i <len; ++i) {
    fprintf(stderr, "%02x", d[i]);
    if ((i & 3) == 3) {fprintf(stderr, " ");}
    if ((i & 15) == 15) {fprintf(stderr, "\n");}
  }
  fprintf(stderr, "\n");
}

static bool first_time = true;
static bool first_error = true;

static uint64 basetime = 0;

void packetHandler(u_char *userData, const struct pcap_pkthdr* pkthdr, const u_char* packet) {
  const struct ether_header* ethernetHeader;
  const struct ip* ipHeader;
  const struct tcphdr* tcpHeader;
  u_char *data;
  int datalength = 0;
  
  uint32 our_ipv4 = *(uint32*)(userData);

//fprintf(stderr, "packetHandler %u: ", pkthdr->len);
//dumpbytes(packet, 64);

  // Ignore too-short packets eth 14(+2) + ip 20 + tcp 20 + rpc 16 + rpcid 4 = 76
  if (pkthdr->len < 76) {return;}

  // Ignore non-IP packets
  // Hack: There are two extra bytes before the ether_type field for some reason
  const u_char* ethstart = packet + 2;
  ethernetHeader = (struct ether_header*)(ethstart);
//fprintf(stderr, "  ether_type %04x\n", ntohs(ethernetHeader->ether_type));
  if (ntohs(ethernetHeader->ether_type) != ETHERTYPE_IP) {return;}

  // Ignore non-TCP packets
  const u_char* ipstart = ethstart + sizeof(struct ether_header);
  ipHeader = (struct ip*)(ipstart);
//fprintf(stderr, "  ip_p %04x\n", ipHeader->ip_p);
  if (ipHeader->ip_p != IPPROTO_TCP) {return;}

  // Pick out the source and dest IPv4 addresses
  bool rx = true;
  uint32 src_ipv4 = *(uint32*)(&ipHeader->ip_src);
  uint32 dst_ipv4 = *(uint32*)(&ipHeader->ip_dst);

  if      (our_ipv4 == src_ipv4) {rx = false;}
  else if (our_ipv4 == dst_ipv4) {rx = true;}
  else {
    if (first_error) {
      fprintf(stderr, "No IP address match. We are %08x, src %08x, dst %08x\n",
        our_ipv4, src_ipv4, dst_ipv4);
      fprintf(stderr, "  Ignoring packet\n");
      first_error = false;
      return;
    }
  }
  

  const u_char* tcpstart = ipstart + (ipHeader->ip_hl * 4);
  tcpHeader = (struct tcphdr*)(tcpstart);
  const u_char* datastart = tcpstart + (tcpHeader->doff * 4);
//dumpbytes(datastart, 32);
  data = (u_char*)(datastart);
  datalength = pkthdr->len - (datastart - packet);
//fprintf(stderr, "  datalength %u\n", datalength);

  // Ignore too-short data rpc 16 + rpcid 4
  if (datalength < 24) {return;}

  const RPCMarker* rpcmarker = (const RPCMarker*)(data);
  // Ignore packets without our signature word at the front
//fprintf(stderr, "  signature %08x %08x %08x %08x\n", 
//rpcmarker->signature, rpcmarker->headerlen, rpcmarker->datalen, rpcmarker->checksum);
  if (!ValidMarker(rpcmarker)) {return;}

  // We have a valid marker, so a likely dclab message beginning

  // Extract the message length and RPCID low 16 bits
  uint32 msg_len = rpcmarker->datalen;
  const RPCHeader* rpcheader = (const RPCHeader*)(data + sizeof(RPCMarker));
  uint32 msg_rpcid = rpcheader->rpcid & 0xFFFF;

  // Write a json line
  // timestamp is seconds within minute and fraction. Seconds must be three chars.
  // This will not line up with KUtrace data if the KUtrace started in a different minute.
  // So we record the first time encountered for tcpalign to fix this later.
  if (first_time) {
    // Put the basetime into the output JSON file. Note leading space.
    first_time = false;
    basetime = (pkthdr->ts.tv_sec / 60) * 60;	// Round down to minute
    struct tm* tmbuf = localtime(&pkthdr->ts.tv_sec);
    fprintf(stdout, " \"tcpdumpba\" : \"%04d-%02d-%02d_%02d:%02d:00\",\n",
      tmbuf->tm_year + 1900, tmbuf->tm_mon + 1, tmbuf->tm_mday,
      tmbuf->tm_hour, tmbuf->tm_min); 
  }
  uint32 ts_seconds = pkthdr->ts.tv_sec - basetime;
  uint32 ts_usec = pkthdr->ts.tv_usec;

  // [ ts, dur,               cpu,   pid, rpcid, event,   arg, ret, ipc, "name"],
  // [ 53.66795600, 0.00000001, 0,   0, rrrr,    516,     length, 0, 0, "rpc.rrrr"],
  uint32 event = rx ? KUTRACE_RPCIDRXMSG : KUTRACE_RPCIDTXMSG;
  fprintf(stdout, "[%3u.%06u00, 0.00000001, 0, 0, %u, %u, %u, 0, 0, \"rpc.%u\"],\n",
    ts_seconds, ts_usec, msg_rpcid, event, msg_len, msg_rpcid); 
}

void usage() {
  fprintf(stderr, "usage: pcaptojson <filename.pcap> <IP addr>\n");
  fprintf(stderr, "example pcaptojson server_tcpdump.pcap 192.168.1.61\n");
  exit(-1);
}

int main(int argc, const char **argv) {
  if(argc != 3) {
    usage();
  }
  const char* filename = argv[1];
  const char* ipstring = argv[2];
  int byte1, byte2, byte3, byte4;
  int n = sscanf(ipstring, "%d.%d.%d.%d", &byte1, &byte2, &byte3, &byte4);
  if (n != 4) {usage();}
  uint32 our_ipv4 = (byte4 << 24) | (byte3 << 16) | (byte2 << 8) | (byte1 << 0);

  pcap_t *fp;
  char errbuf[PCAP_ERRBUF_SIZE];
    //char source[PCAP_BUF_SIZE];
    //int i, maxCountSyn = 0, maxCountHttp = 0, maxIdxSyn = 0, maxIdxHttp = 0;

  fp = pcap_open_offline(filename, errbuf);
  if (fp == NULL) {
    fprintf(stderr, "\npcap_open_offline() failed: %s\n", errbuf);
    return 0;
  }

  if (pcap_loop(fp, 0, packetHandler, (u_char*)(&our_ipv4)) < 0) {
    fprintf(stderr, "\npcap_loop() failed: %s\n", pcap_geterr(fp));
    return 0;
  }

  return 0;
}




