#!/usr/bin/gawk -f

BEGIN {
    # If variables weren't passed via -v, set defaults
    if (!Port) Port = "6379"
    if (!RespFile) RespFile = "rsp_file.txt"
    Service = "/inet/tcp/" Port "/0/0"

    while (1) {
        if ((Service |& getline client_line) > 0) {
            # Log incoming
            print client_line
            fflush(); 

            # Read and send response
            if ((getline resp_line < RespFile) > 0) {
                print resp_line |& Service
            } else {
                print "ERROR: NO_MORE_RESPONSES" |& Service
            }
            fflush(Service)
        } else {
            close(Service)
            close(RespFile) # Resets to top of file for next client
        }
    }
}

