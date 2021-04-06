#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include "FIFOreqchannel.h"

using namespace std;

// For which patient? Push n requests into BoundedBuffer requestBuffer
void patient_thread_function(int p, int n, BoundedBuffer* requestBuffer){ 
    /* What will the patient threads do? */ 
    // same thing we did in PA1 for 1000 requests
    datamsg d(p, 0.00, 1);
    for (int i=0; i<n; i++) {
        requestBuffer -> push( (char*)&d, sizeof(datamsg) );
        d.seconds += 0.004; //update timestamp of request by 4 ms
    }
}

struct Response{
    int person;
    double ecg;
};

void worker_thread_function(BoundedBuffer* requestBuffer, FIFORequestChannel* wchan, BoundedBuffer* responseBuffer, int mb){
    /* Infinite loop that keeps the thread moving by popping a request and processing the request by sending it to the server,
        receiving the response, and pushing the response onto the responseBuffer  */
    char buf [1024];
    char recvbuf[mb];
    while (true) { 
        requestBuffer->pop (buf, sizeof(buf) );
        MESSAGE_TYPE* m = (MESSAGE_TYPE*)buf;
        if (*m == QUIT_MSG) {
            wchan->cwrite(m, sizeof(MESSAGE_TYPE));
            delete wchan;
            break;
        }
        if (*m == DATA_MSG) {
            datamsg* d = (datamsg*) buf;
            wchan->cwrite(d, sizeof(datamsg));
            double ecg;
            wchan->cread(&ecg, sizeof (double));
            Response r{d->person, ecg};
            responseBuffer->push((char*)&r, sizeof(r) );
        }
        else if (*m == FILE_MSG) {
            /* send the file request, get the response back, and when you get the response you
            dont have to push to the responseBuffer at all- you just straight away write to the file */
            filemsg* fm = (filemsg*) buf;
            string filename = (char*)(fm+1);

            int sz = sizeof(filemsg) + filename.size()+1; 
            wchan->cwrite(buf,sz);
            wchan->cread(recvbuf, mb);

            // Write recvbuf to file
            string recvfname = "recv/"+filename;
            FILE* fp = fopen(recvfname.c_str(), "r+");

            fseek(fp, fm->offset, SEEK_SET); //moves pointer to offset position

            // Only writes their specific portion, so we don't need to think abouit synchronization since
            // they wont overlap
            fwrite(recvbuf, 1, fm->length, fp);

            // Close file
            fclose(fp);
        }
    }
}
void histogram_thread_function (BoundedBuffer* responseBuffer, HistogramCollection* hc){
    char buf[1024];
    while (true) {
        responseBuffer->pop(buf, sizeof(buf));

        // When we called pop, it copied the data as a char* into "buf", so we need to convert it back to a Response
        Response* r = (Response*) buf;
        if (r->person == -1) { // Termination condition
            break;
        }
        hc->update(r->person, r->ecg);
    }
}

void file_thread_function(string fname, BoundedBuffer* requestBuffer, FIFORequestChannel* chan, int mb) {
    // Perform a single hardcoded file transfer to obtain the size of the file
    char buf[1024];
    filemsg f(0,0);
    memcpy(buf, &f, sizeof(f));
    strcpy(buf+sizeof(f),fname.c_str());
    chan->cwrite(buf, sizeof(f)+fname.size()+1);
    __int64_t filelength;
    chan->cread(&filelength, sizeof(filelength));

    // Create an output file, empty but of size filelength
    string recvfname = "recv/" + fname;
    FILE *fp = fopen(recvfname.c_str(), "w");
    fseek(fp, filelength, SEEK_SET); // Move pointer from 0 to the right of filelength
    fclose(fp);

    // General all filemsgs and push to requestBuffer
    filemsg *fm = (filemsg*) buf;
    __int64_t remlen = filelength;
    while (remlen>0) {
        fm->length = min(remlen, (__int64_t)mb);

        requestBuffer->push(buf, sizeof(filemsg)+fname.size()+1);
        fm->offset += fm->length;
        remlen -= fm->length;
    }
}

int main(int argc, char *argv[])
{
    int n = 100;    //default number of requests per "patient"
    int p = 15;     // number of patients [1,15]
    int w = 50;    //default number of worker threads
    int b = 1024; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
    srand(time_t(NULL));
    int h = 5;      // number of histogram threads
    string filename;
    bool isfiletransfer = false;
    
    // Getting command line arguments
    int c;
    while ((c = getopt (argc, argv, "n:p:w:b:m:f:h:")) != -1){
        switch (c){
            case 'n': // Number of requests per patient
                n = atoi (optarg);
                break;
            case 'p': // Number of patients
                p = atoi (optarg);
                break;
            case 'w': // Number of worker threads
                w = atoi (optarg);
                break;
            case 'b': // Capacity of request buffer
                b = atoi (optarg);
                break;
            case 'm':
                m = atoi (optarg);
                break;
            case 'f':
                isfiletransfer = true;
                filename = optarg;
                break;
            case 'h': // Number of histogram threads
                h = atoi(optarg);
                break;
        }
    }
    
    int pid = fork();
    if (pid == 0){
		// modify this to pass along m
        execl ("server", "server", (char *)NULL);
    }
    
	FIFORequestChannel* chan = new FIFORequestChannel("control", FIFORequestChannel::CLIENT_SIDE);
    BoundedBuffer requestBuffer(b);
    BoundedBuffer responseBuffer(b);
	HistogramCollection hc;

    // Create w worker CHANNELS
    FIFORequestChannel* wchans [w];
    for (int i=0; i<w; i++) {
        MESSAGE_TYPE m = NEWCHANNEL_MSG;
        chan->cwrite (&m, sizeof(m));
        char newchanname [100];
        chan->cread (newchanname, sizeof(newchanname));
        wchans[i] = new FIFORequestChannel (newchanname, FIFORequestChannel::CLIENT_SIDE);
    }
	
    // Make histograms and adding to the histogram collection hc
    for (int i=0; i<p; i++){
        Histogram* h = new Histogram (10,-2.0, 2.0); // Set bins here
        hc.add(h);
    }

    // Start Stopwatch
    struct timeval start, end;
    gettimeofday (&start, 0);
    
    /* Start all threads here -----------------------------------------------------------------------*/
    thread patients [p];
    thread workers [w];
    thread hists [h];

    // Only for file transfers
    if (isfiletransfer) {
        thread filethread(file_thread_function, filename, &requestBuffer, chan, m);

    } else { // Not a file transfer
        // Create p patient threads 
        
        for (int i=0; i<p; i++) {
            patients [i] = thread (patient_thread_function, i+1, n, &requestBuffer );
        }
        // Create h histogram threads
        for (int i=0; i<h; i++) {
            hists [i] = thread (histogram_thread_function, &responseBuffer, &hc );
        }
    }
    
    // Create w worker threads
    for (int i=0; i<w; i++) {
        workers [i] = thread (worker_thread_function, &requestBuffer, wchans[i], &responseBuffer, m);
    }
    
    
    /* Join all threads here (Kill each part of the pipeline) */
        if (isfiletransfer) {
            // Patient threads will finish first, so then we must push a QUIT_MSG so that the workers find it
            for (int i=0; i<p; i++) {
                patients[i].join(); } // Way to wait for a thread to finish
        }
        
        for (int i=0; i<w; i++) {
            MESSAGE_TYPE q = QUIT_MSG;
            requestBuffer.push ((char*)&q, sizeof(MESSAGE_TYPE));
        }
        //filethread.join();
        for (int i=0; i<w; i++) 
            workers[i].join(); // Workers are done here
        //Send kill signal to histogram threads
        Response r {-1,0};
        for (int i=0; i<h; i++) {
            responseBuffer.push((char*)&r, sizeof(r));
        }
        for (int i=0; i<h; i++)
            hists[i].join(); // Histograms die one after another
            

	// End stopwatch
    gettimeofday (&end, 0);

    // Print the results
	hc.print ();

    // Format the time difference we recorded
    int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
    int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
    cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

    // Tell the server we're done
    MESSAGE_TYPE q = QUIT_MSG;
    chan->cwrite ((char *) &q, sizeof (MESSAGE_TYPE));
    cout << "All Done!!!" << endl;
    wait (0); // Make sure the server has enough time to quit the channels
    delete chan;
    
}
