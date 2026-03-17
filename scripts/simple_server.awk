#!/usr/bin/gawk -f

BEGIN {
    # If variables weren't passed via -v, set defaults
    if (!Port) Port = "6379"
    if (!RespFile) RespFile = "rsp_file.txt"
    if (!LogFile) LogFile = "recv_file.txt"
    
    Service = "/inet/tcp/" Port "/0/0"

    print "Server starting on port " Port "..."
    print "Logging to: " LogFile
    print "Responding with: " RespFile

    while (1) {
        if ((Service |& getline client_line) > 0) {
            # Log incoming
            print client_line >> LogFile
            fflush(LogFile)

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

