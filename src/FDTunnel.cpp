// For all support, instructions and copyright go to:

// http://e2guardian.org/ll
// Released under the GPL v2, with the OpenSSL exception described in the README file.

// This class is a generic multiplexing tunnel
// that uses blocking select() to be as efficient as possible.  It tunnels
// between the two supplied FDs.

// INCLUDES

#ifdef HAVE_CONFIG_H
#include "dgconfig.h"
#endif

#include <sys/time.h>
#include <unistd.h>
#include <stdexcept>
#include <cerrno>
#include <sys/socket.h>
#include <string.h>
#include <algorithm>
#include <sys/select.h>

#ifdef DGDEBUG
#include <iostream>
#endif

#include "FDTunnel.hpp"

// IMPLEMENTATION

extern thread_local std::string thread_id;

FDTunnel::FDTunnel()
    : throughput(0)
{
}

void FDTunnel::reset()
{
    throughput = 0;
}

// tunnel data from fdfrom to fdto (unfiltered)
// return false if throughput larger than target throughput
bool FDTunnel::tunnel(Socket &sockfrom, Socket &sockto, bool twoway, off_t targetthroughput, bool ignore, bool chunked)
{
    if (chunked) {
#ifdef DGDEBUG
    std::cout << thread_id << "tunnelling chunked data." << std::endl;
#endif
        int maxlen = 32000;
        char buff[32000];
        int timeout = sockfrom.getTimeout();
        int rd = 0;
        int total_rd = 0;
        while ( (rd = sockfrom.readChunk(buff,maxlen,timeout)) > 0) {
            sockto.writeChunk(buff, rd, timeout);
            total_rd += rd;
        }
        sockto.writeChunkTrailer(sockfrom.chunked_trailer);
        throughput = total_rd;
        return true;
    }
    if (targetthroughput == 0) {
#ifdef DGDEBUG
        std::cout << thread_id << "No data expected, tunnelling aborted." << std::endl;
#endif
        return true;
    }

#ifdef DGDEBUG
    if (targetthroughput < 0)
        std::cout <<thread_id << "Tunnelling without known content-length" << std::endl;
    else
        std::cout <<thread_id << "Tunnelling with content length " << targetthroughput << std::endl;
#endif
    if ((sockfrom.bufflen - sockfrom.buffstart) > 0) {
#ifdef DGDEBUG
        std::cout <<thread_id << "Data in fdfrom's buffer; sending " << (sockfrom.bufflen - sockfrom.buffstart) << " bytes" << std::endl;
#endif
        if (!sockto.writeToSocket(sockfrom.buffer + sockfrom.buffstart, sockfrom.bufflen - sockfrom.buffstart, 0, 120000, false))
            return false;
           // throw std::runtime_error(std::string("Can't write to socket: ") + strerror(errno));
#ifdef DGDEBUG
        std::cout <<thread_id << "Data in fdfrom's buffer; sent " << (sockfrom.bufflen - sockfrom.buffstart) << " bytes" << std::endl;
#endif

        throughput += sockfrom.bufflen - sockfrom.buffstart;
        sockfrom.bufflen = 0;
        sockfrom.buffstart = 0;
    }

    int rc, fdfrom, fdto;

    fdfrom = sockfrom.getFD();
    fdto = sockto.getFD();
    fromoutfds[0].fd = fdfrom;
    fromoutfds[0].events = POLLOUT;
    tooutfds[0].fd = fdto;
    tooutfds[0].events = POLLOUT;
    twayfds[0].fd = fdfrom;
    twayfds[0].events = POLLIN;
    twayfds[1].events = POLLIN;
    if (ignore && !twoway) {
        twayfds[1].fd = -1;
        twayfds[1].revents = 0;
    }
    else
        twayfds[1].fd = fdto;

    char buff[32768]; // buffer for the input
    int timeout = 120000;    // should be made setable in conf files

    bool done = false; // so we get past the first while

    while (!done && (targetthroughput > -1 ? throughput < targetthroughput : true)) {
        done = true; // if we don't make a sucessful read and write this
        // flag will stay true and so the while() will exit
#ifdef DGDEBUG
        std::cout <<thread_id << "Start of tunnel loop: throughput:" << throughput
            << " target:"  << targetthroughput  << std::endl;
#endif

        //FD_CLR(fdto, &inset);

#ifdef __SSLMITM
        // TODO: Post v5.3 change socket logic to non-blocking so that poll can be used in MITM
        // after read/write - PP
     if (sockfrom.isSsl()) {
      twayfds[0].revents = POLLIN;
      twayfds[1].revents = 0;
  } else
#endif
        {
            int rc = poll(twayfds, 2, timeout);
            if (rc < 1) {
#ifdef DGDEBUG
                std::cout <<thread_id << "tunnel tw poll returned error or timeout::" << rc
                  << std::endl;
#endif
                break; // an error occurred or it timed out so end while()
            }
#ifdef DGDEBUG
            std::cout <<thread_id << "tunnel tw poll returned ok:" << rc
                  << std::endl;
#endif
                  }

            if (twayfds[0].revents & (POLLIN | POLLHUP))
            {
                if (targetthroughput > -1)
                    // we have a target throughput - only read in the exact amount of data we've been told to
                    // plus 2 bytes to "solve" an IE post bug with multipart/form-data forms:
                    // adds an extra CRLF on certain requests, that it doesn't count in reported content-length
                    rc = sockfrom.readFromSocket(buff, (((int)sizeof(buff) < ((targetthroughput - throughput) /*+2*/)) ? sizeof(buff) : (targetthroughput - throughput) /* + 2*/), 0, 0, false);
                else
                    rc = sockfrom.readFromSocket(buff, sizeof(buff), 0, 0, false);

                // read as much as is available
                if (rc < 0) {
                    break; // an error occurred so end the while()
                } else if (!rc) {
                    done = true; // none received so pipe is closed so flag it
                } else { // some data read
#ifdef DGDEBUG
                    std::cout <<thread_id << "tunnel got data from sockfrom: " << rc << " bytes"
                        << std::endl;
#endif
                    throughput += rc; // increment our counter used to log
                    if (poll (tooutfds,1, timeout ) < 1)
                     {
                        break; // an error occurred or timed out so end while()
                    }

                     if (tooutfds[0].revents & POLLOUT)
                        {
                            if (!sockto.writeToSocket(buff, rc, 0, 0, false)) { // write data
                            break; // was an error writing
                            }
#ifdef DGDEBUG
                         std::cout <<thread_id << "tunnel wrote data out: " << rc << " bytes"
                        << std::endl;
#endif
                        done = false; // flag to say data still to be handled
                    } else {
                        break; // should never get here
                    }
                }
            }
            if ( twayfds[1].revents & (POLLIN | POLLHUP))
            {
                if (!twoway) {
    // since HTTP works on a simple request/response basis, with no explicit
    // communications from the client until the response has been completed
    // (just TCP cruft, which is of no interest to us here), tunnels only
    // need to be one way. As soon as the client tries to send data, break
    // the tunnel, as it will be a new request, possibly to an entirely
    // different webserver. PRA 2005-11-14
#ifdef DGDEBUG
                    std::cout <<thread_id << "fdto is sending data; closing tunnel. (This must be a persistent connection.)" << std::endl;
#endif
                    break;
                }

                // read as much as is available
                rc = sockto.readFromSocket(buff, sizeof(buff), 0, 0, false);

                if (rc < 0) {
                    break; // an error occurred so end the while()
                } else if (!rc) {
                    done = true; // none received so pipe is closed so flag it
                    break;
                } else { // some data read
                    if (poll (fromoutfds,1, timeout ) < 1)
                    {
                        break; // an error occurred or timed out so end while()
                    }

                        if (fromoutfds[0].revents & POLLOUT)
                        {
                        if (!sockfrom.writeToSocket(buff, rc, 0, 0, false)) { // write data
                            break; // was an error writing
                        }
                        done = false; // flag to say data still to be handled
                    } else {
                        break; // should never get here
                    }
                }
            }
        }
#ifdef DGDEBUG
        if ((throughput >= targetthroughput) && (targetthroughput > -1))
            std::cout <<thread_id << "All expected data tunnelled. (expected " << targetthroughput << "; tunnelled " << throughput << ")" << std::endl;
        else
            std::cout <<thread_id << "Tunnel closed." << std::endl;
#endif
        return (targetthroughput > -1) ? (throughput >= targetthroughput) : true;
    }
