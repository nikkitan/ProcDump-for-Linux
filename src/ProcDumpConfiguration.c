// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

//--------------------------------------------------------------------
//
// The global configuration structure and utilities header
//
//--------------------------------------------------------------------

#include "Procdump.h"
#include "ProcDumpConfiguration.h"

struct Handle g_evtConfigurationInitialized = HANDLE_MANUAL_RESET_EVENT_INITIALIZER("ConfigurationInitialized");

static sigset_t sig_set;
static pthread_t sig_thread_id;

//--------------------------------------------------------------------
//
// SignalThread - Thread for hanlding graceful Async signals (e.g., SIGINT, SIGTERM)
//
//--------------------------------------------------------------------
void *SignalThread(void *input)
{
    struct ProcDumpConfiguration *self = (struct ProcDumpConfiguration *)input;
    int sig_caught, rc;

    if ((rc = sigwait(&sig_set, &sig_caught)) != 0) {
        Log(error, "Failed to wait on signal");
        exit(-1);
    }
    
    switch (sig_caught)
    {
    case SIGINT:
        SetQuit(self, 1);
        if(self->gcorePid != NO_PID) {
            Log(info, "Shutting down gcore");
            if((rc = kill(-self->gcorePid, SIGKILL)) != 0) {            // pass negative PID to kill entire PGRP with value of gcore PID
                Log(error, "Failed to shutdown gcore.");
            }
        }
        Log(info, "Quit");
        break;
    default:
        fprintf (stderr, "\nUnexpected signal %d\n", sig_caught);
        break;
    }

    pthread_exit(NULL);
}

//--------------------------------------------------------------------
//
// InitProcDump - initalize procdump
//
//--------------------------------------------------------------------
void InitProcDump()
{
    InitProcDumpConfiguration(&g_config);
    openlog("ProcDump", LOG_PID, LOG_USER);
    pthread_mutex_init(&LoggerLock, NULL);
}

//--------------------------------------------------------------------
//
// ExitProcDump - cleanup during exit. 
//
//--------------------------------------------------------------------
void ExitProcDump()
{
    pthread_mutex_destroy(&LoggerLock);
    closelog();
    FreeProcDumpConfiguration(&g_config);
}

//--------------------------------------------------------------------
//
// InitProcDumpConfiguration - initalize a config
//
//--------------------------------------------------------------------
void InitProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    if (WaitForSingleObject(&g_evtConfigurationInitialized, 0) == WAIT_OBJECT_0) {
        return; // The configuration has already been initialized
    }

    MAXIMUM_CPU = 100 * (int)sysconf(_SC_NPROCESSORS_ONLN);
    HZ = sysconf(_SC_CLK_TCK);

    sysinfo(&(self->SystemInfo));

    InitNamedEvent(&(self->evtCtrlHandlerCleanupComplete.event), true, false, "CtrlHandlerCleanupComplete");
    self->evtCtrlHandlerCleanupComplete.type = EVENT;

    InitNamedEvent(&(self->evtBannerPrinted.event), true, false, "BannerPrinted");
    self->evtBannerPrinted.type = EVENT;

    InitNamedEvent(&(self->evtConfigurationPrinted.event), true, false, "ConfigurationPrinted");
    self->evtConfigurationPrinted.type = EVENT;

    InitNamedEvent(&(self->evtDebugThreadInitialized.event), true, false, "DebugThreadInitialized");
    self->evtDebugThreadInitialized.type = EVENT;

    InitNamedEvent(&(self->evtQuit.event), true, false, "Quit");
    self->evtQuit.type = EVENT;

    InitNamedEvent(&(self->evtStartMonitoring.event), true, false, "StartMonitoring");
    self->evtStartMonitoring.type = EVENT;

    sem_init(&(self->semAvailableDumpSlots.semaphore), 0, 1);
    self->semAvailableDumpSlots.type = SEMAPHORE;

    // Additional initialization
    self->ProcessId =                   NO_PID;
    self->NumberOfDumpsCollected =      0;
    self->NumberOfDumpsToCollect =      DEFAULT_NUMBER_OF_DUMPS;
    self->CpuThreshold =                -1;
    self->MemoryThreshold =             -1;
    self->ThresholdSeconds =            DEFAULT_DELTA_TIME;
    self->bCpuTriggerBelowValue =       false;
    self->bMemoryTriggerBelowValue =    false;
    self->bTimerThreshold =             false;
    self->WaitingForProcessName =       false;
    self->DiagnosticsLoggingEnabled =   false;
    self->gcorePid = NO_PID;

    SetEvent(&g_evtConfigurationInitialized.event); // We've initialized and are now re-entrant safe
}


//--------------------------------------------------------------------
//
// FreeProcDumpConfiguration - ensure destruction of config and contents
//
//--------------------------------------------------------------------
void FreeProcDumpConfiguration(struct ProcDumpConfiguration *self)
{
    DestroyEvent(&(self->evtCtrlHandlerCleanupComplete.event));
    DestroyEvent(&(self->evtBannerPrinted.event));
    DestroyEvent(&(self->evtConfigurationPrinted.event));
    DestroyEvent(&(self->evtDebugThreadInitialized.event));
    DestroyEvent(&(self->evtQuit.event));
    DestroyEvent(&(self->evtStartMonitoring.event));

    sem_destroy(&(self->semAvailableDumpSlots.semaphore));

    free(self->ProcessName);
}

//--------------------------------------------------------------------
//
// GetOptions - Unpack command line inputs
//
//--------------------------------------------------------------------
int GetOptions(struct ProcDumpConfiguration *self, int argc, char *argv[])
{
    // Make sure config has been initialized
    if (WaitForSingleObject(&g_evtConfigurationInitialized, 0) != WAIT_OBJECT_0) {
        Trace("GetOptions: Configuration not initialized.");
        return -1;
    }

    if (argc < 2) {
        Trace("GetOptions: Invalid number of command line arguments.");
        return PrintUsage(self);
    }

    // parse arguments
	int next_option;
    int option_index = 0;
    const char* short_options = "+p:C:c:M:m:n:s:w:dh";
    const struct option long_options[] = {
    	{ "pid",                       required_argument,  NULL,           'p' },
    	{ "cpu",                       required_argument,  NULL,           'C' },
    	{ "lower-cpu",                 required_argument,  NULL,           'c' },
    	{ "memory",                    required_argument,  NULL,           'M' },
    	{ "lower-mem",                 required_argument,  NULL,           'm' },
        { "number-of-dumps",           required_argument,  NULL,           'n' },
        { "time-between-dumps",        required_argument,  NULL,           's' },
        { "wait",                      required_argument,  NULL,           'w' },
        { "diag",                      no_argument,        NULL,           'd' },
        { "help",                      no_argument,        NULL,           'h' }
    };

    // start parsing command line arguments
    while ((next_option = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (next_option) {
            case 'p':
                self->ProcessId = (pid_t)atoi(optarg);
                if (!LookupProcessByPid(self)) {
                    Log(error, "Invalid PID - failed looking up process name by PID.");
                    return PrintUsage(self);
                }
                break;

            case 'C':
                if (self->CpuThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->CpuThreshold = atoi(optarg)) < 0 || self->CpuThreshold > MAXIMUM_CPU) {
                    Log(error, "Invalid CPU threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'c':
                if (self->CpuThreshold != -1 || !IsValidNumberArg(optarg) ||
                    (self->CpuThreshold = atoi(optarg)) < 0 || self->CpuThreshold > MAXIMUM_CPU) {
                    Log(error, "Invalid CPU threshold specified.");
                    return PrintUsage(self);
                }
                self->bCpuTriggerBelowValue = true;
                break;

            case 'M':
                if (self->MemoryThreshold != -1 || 
                    !IsValidNumberArg(optarg) ||
                    (self->MemoryThreshold = atoi(optarg)) < 0) {
                    Log(error, "Invalid memory threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'm':
                if (self->MemoryThreshold != -1 || 
                    !IsValidNumberArg(optarg) ||
                    (self->MemoryThreshold = atoi(optarg)) < 0) {
                    Log(error, "Invalid memory threshold specified.");
                    return PrintUsage(self);
                }            
                self->bMemoryTriggerBelowValue = true;
                break;

            case 'n':
                if (!IsValidNumberArg(optarg) ||
                    (self->NumberOfDumpsToCollect = atoi(optarg)) < 0) {
                    Log(error, "Invalid dumps threshold specified.");
                    return PrintUsage(self);
                }                
                break;

            case 's':
                if (!IsValidNumberArg(optarg) ||
                    (self->ThresholdSeconds = atoi(optarg)) == 0) {
                    Log(error, "Invalid time threshold specified.");
                    return PrintUsage(self);
                }
                break;

            case 'w':
                self->WaitingForProcessName = true;
                self->ProcessName = strdup(optarg);
                break;

            case 'd':
                self->DiagnosticsLoggingEnabled = true;
                break;
                
            case 'h':
                return PrintUsage(self);

            default:
                Log(error, "Invalid switch specified");
                return PrintUsage(self);
        }
    }

    // Check for multi-arg situations

    // if number of dumps is set, but no thresholds, just go on timer
    if (self->NumberOfDumpsToCollect != -1 &&
        self->MemoryThreshold == -1 &&
        self->CpuThreshold == -1) {
            self->bTimerThreshold = true;
        }

    if(self->ProcessId == NO_PID && !self->WaitingForProcessName){
        Log(error, "A valid PID or process name must be specified");
        return PrintUsage(self);
    }

    if(self->ProcessId != NO_PID && self->WaitingForProcessName){
	Log(error, "Please only specify one of -p or -w");
	return PrintUsage(self);
    }

    if(!self->WaitingForProcessName) {
        self->ProcessName = GetProcessName(self->ProcessId);
        if (self->ProcessName == NULL) {
            Log(error, "Error getting process name.");
	    }
    }

    Trace("GetOpts and initial Configuration finished");

    return 0;
}


//--------------------------------------------------------------------
//
// LookupProcessByPid - Find process using PID provided.  
//
//--------------------------------------------------------------------
bool LookupProcessByPid(struct ProcDumpConfiguration *self)
{
    char statFilePath[32];

    // check to see if pid is an actual process running
    sprintf(statFilePath, "/proc/%d/stat", self->ProcessId);
    FILE *fd = fopen(statFilePath, "r");
    if (fd == NULL) {
        Log(error, "No process matching the specified PID can be found.");
        Log(error, "Try elevating the command prompt (i.e., `sudo procdump ...`)");
        return false;
    }

    // close file pointer this is a valid process
    fclose(fd);
    return true;
}


//--------------------------------------------------------------------
//
// FilterForPid - Helper function for scandir to only return PIDs.
//
//--------------------------------------------------------------------
static int FilterForPid(const struct dirent *entry)
{
    return IsValidNumberArg(entry->d_name);
}

//--------------------------------------------------------------------
//
// WaitForProcessName - Actively wait until a process with the configured name is launched.
//
//--------------------------------------------------------------------
bool WaitForProcessName(struct ProcDumpConfiguration *self)
{
    Log(info, "Waiting for process '%s' to launch...", self->ProcessName);
    while (true) {
        struct dirent ** nameList;
        bool moreThanOne = false;
        pid_t matchingPid = NO_PID;
        int numEntries = scandir("/proc/", &nameList, FilterForPid, alphasort);
        for (int i = 0; i < numEntries; i++) {
            pid_t procPid = atoi(nameList[i]->d_name);
            char *nameForPid = GetProcessName(procPid);
            if (nameForPid == NULL) {
                continue;
            }
            if (strcmp(nameForPid, self->ProcessName) == 0) {
                if (matchingPid == NO_PID) {
                    matchingPid = procPid;
                } else {
                    Log(error, "More than one matching process found, exiting...");
                    moreThanOne = true;
                    free(nameForPid);
                    break;
                }
            }
            free(nameForPid);
        }
        // Cleanup
        for (int i = 0; i < numEntries; i++) {
            free(nameList[i]);
        }
        free(nameList);

        // Check for exactly one match
        if (moreThanOne) {
            self->bTerminated = true;
            return false;
        } else if (matchingPid != NO_PID) {
            self->ProcessId = matchingPid;
            Log(info, "Found process with PID %d", matchingPid);
            return true;
        }
    }
}

//--------------------------------------------------------------------
//
// GetProcessName - Get process name using PID provided  
//
//--------------------------------------------------------------------
char * GetProcessName(pid_t pid){
	char procFilePath[32];
	char fileBuffer[MAX_CMDLINE_LEN + 1] = {0};		// maximum command line length on Linux
	int charactersRead = 0;
	int	itr = 0;
	char * stringItr;
	char * processName;
	FILE * procFile;
	
	if(sprintf(procFilePath, "/proc/%d/cmdline", pid) < 0){
		return NULL;
	}
	procFile = fopen(procFilePath, "r");

	if(procFile != NULL){
		if((charactersRead = fread(fileBuffer, sizeof(char), MAX_CMDLINE_LEN, procFile)) == 0) {
			Log(debug, "Failed to read from %s.\n", procFilePath);
			fclose(procFile);
			return NULL;
		}
	
		// close file
		fclose(procFile);
	}
	else{
		Log(debug, "Failed to open %s.\n", procFilePath);
		return NULL;
	}

	// Extract process name
	stringItr = fileBuffer;
	for(int i = 0; i < charactersRead + 1; i++){
		if(fileBuffer[i] == '\0'){
			itr = i - itr;
			
			if(strcmp(stringItr, "sudo") != 0){		// do we have the process name including filepath?
				processName = strrchr(stringItr, '/');	// does this process include a filepath?
				
				if(processName != NULL){
					return strdup(processName + 1);	// +1 to not include '/' character
				}
				else{
					return strdup(stringItr);
				}
			}
			else{
				stringItr += (itr+1); 	// +1 to move past '\0'
			}
		}
	}

	Log(debug, "Failed to extract process name from /proc/PID/cmdline");
	return NULL;
}

//--------------------------------------------------------------------
//
// CreateTriggerThreads - Create each of the threads that will be running as a trigger 
//
//--------------------------------------------------------------------
int CreateTriggerThreads(struct ProcDumpConfiguration *self)
{    
    int rc = 0;
    self->nThreads = 0;

    if((rc=sigemptyset (&sig_set)) < 0)
    {
        Trace("CreateTriggerThreads: sigemptyset failed.");
        return rc;
    }
    if((rc=sigaddset (&sig_set, SIGINT)) < 0)
    {
        Trace("CreateTriggerThreads: sigaddset failed.");
        return rc;
    }
    if((rc=sigaddset (&sig_set, SIGTERM)) < 0)
    {
        Trace("CreateTriggerThreads: sigaddset failed.");
        return rc;
    }

    if((rc = pthread_sigmask (SIG_BLOCK, &sig_set, NULL)) != 0)
    {
        Trace("CreateTriggerThreads: pthread_sigmask failed.");
        return rc;
    }

    // create threads
    if (self->CpuThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, CpuThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create CpuThread.");            
            return rc;
        }
    }

    if (self->MemoryThreshold != -1) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, CommitThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create CommitThread.");            
            return rc;
        }
    }

    if (self->bTimerThreshold) {
        if ((rc = pthread_create(&self->Threads[self->nThreads++], NULL, TimerThread, (void *)self)) != 0) {
            Trace("CreateTriggerThreads: failed to create TimerThread.");
            return rc;
        }
    }
    
    if((rc = pthread_create(&sig_thread_id, NULL, SignalThread, (void *)self))!= 0)
    {
        Trace("CreateTriggerThreads: failed to create SignalThread.");
        return rc;
    }

    return 0;
}


//--------------------------------------------------------------------
//
// WaitForQuit - Wait for Quit Event or just timeout
//
//      Timed wait with awareness of quit event  
//
// Returns: WAIT_OBJECT_0   - Quit triggered
//          WAIT_TIMEOUT    - Timeout
//          WAIT_ABANDONED  - At dump limit or terminated
//
//--------------------------------------------------------------------
int WaitForQuit(struct ProcDumpConfiguration *self, int milliseconds)
{
    if (!ContinueMonitoring(self)) {
        return WAIT_ABANDONED; 
    }

    int wait = WaitForSingleObject(&self->evtQuit, milliseconds);

    if ((wait == WAIT_TIMEOUT) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED; 
    }

    return wait;
}


//--------------------------------------------------------------------
//
// WaitForQuitOrEvent - Wait for Quit Event, an Event, or just timeout
//
//      Use to wait for dumps to complete, yet be aware of quit or finished events 
//
// Returns: WAIT_OBJECT_0   - Quit triggered
//          WAIT_OBJECT_0+1 - Event triggered
//          WAIT_TIMEOUT    - Timeout
//          WAIT_ABANDONED  - (Abandonded) At dump limit or terminated
//
//--------------------------------------------------------------------
int WaitForQuitOrEvent(struct ProcDumpConfiguration *self, struct Handle *handle, int milliseconds)
{
    struct Handle *waits[2];
    waits[0] = &self->evtQuit;
    waits[1] = handle;

    if (!ContinueMonitoring(self)) {
        return WAIT_ABANDONED; 
    }

    int wait = WaitForMultipleObjects(2, waits, false, milliseconds);
    if ((wait == WAIT_TIMEOUT) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED; 
    }

    if ((wait == WAIT_OBJECT_0) && !ContinueMonitoring(self)) {
        return WAIT_ABANDONED;
    }

    return wait;
}


//--------------------------------------------------------------------
//
// WaitForAllThreadsToTerminate - Wait for all threads to terminate  
//
//--------------------------------------------------------------------
int WaitForAllThreadsToTerminate(struct ProcDumpConfiguration *self)
{
    int rc = 0;
    for (int i = 0; i < self->nThreads; i++) {
        if ((rc = pthread_join(self->Threads[i], NULL)) != 0) {
            Log(error, "An error occured while joining threads\n");
            exit(-1);
        }
    }
    if ((rc = pthread_cancel(sig_thread_id)) != 0) {
        Log(error, "An error occured while canceling SignalThread.\n");
        exit(-1);
    }
    if ((rc = pthread_join(sig_thread_id, NULL)) != 0) {
        Log(error, "An error occured while joining SignalThread.\n");
        exit(-1);
    }
    return rc;
}

//--------------------------------------------------------------------
//
// IsQuit - A check on the underlying value of whether we should quit  
//
//--------------------------------------------------------------------
bool IsQuit(struct ProcDumpConfiguration *self)
{
    return (self->nQuit != 0);
}


//--------------------------------------------------------------------
//
// SetQuit - Sets the quit value and signals the event 
//
//--------------------------------------------------------------------
int SetQuit(struct ProcDumpConfiguration *self, int quit)
{
    self->nQuit = quit;
    SetEvent(&self->evtQuit.event);

    return self->nQuit;
}


//--------------------------------------------------------------------
//
// PrintConfiguration - Prints the current configuration to the command line
//
//--------------------------------------------------------------------
bool PrintConfiguration(struct ProcDumpConfiguration *self)
{
    if (WaitForSingleObject(&self->evtConfigurationPrinted,0) == WAIT_TIMEOUT) {
        printf("Process:\t\t%s", self->ProcessName);
        if (!self->WaitingForProcessName) {
            printf(" (%d)", self->ProcessId);
        } else {
            printf(" (pending)");
        }
        printf("\n");

        // CPU
        if (self->CpuThreshold != -1) {
            if (self->bCpuTriggerBelowValue) {
                printf("CPU Threshold:\t\t<%d\n", self->CpuThreshold);
            } else {
                printf("CPU Threshold:\t\t>=%d\n", self->CpuThreshold);
            }
        } else {
            printf("CPU Threshold:\t\tn/a\n");
        }

        // Memory
        if (self->MemoryThreshold != -1) {
            if (self->bMemoryTriggerBelowValue) {
                printf("Commit Threshold:\t<%d\n", self->MemoryThreshold);
            } else {
                printf("Commit Threshold:\t>=%d\n", self->MemoryThreshold);
            }
        } else {
            printf("Commit Threshold:\tn/a\n");
        }

        // time
        printf("Threshold Seconds:\t%d\n", self->ThresholdSeconds);

        // number of dumps and others
        printf("Number of Dumps:\t%d\n", self->NumberOfDumpsToCollect);

        SetEvent(&self->evtConfigurationPrinted.event);
        return true;
    }
    return false;
}


//--------------------------------------------------------------------
//
// ContinueMonitoring - Should we keep monitoring or should we clean up our thread 
//
//--------------------------------------------------------------------
bool ContinueMonitoring(struct ProcDumpConfiguration *self)
{
    // Have we reached the dump limit?
    if (self->NumberOfDumpsCollected >= self->NumberOfDumpsToCollect) {
        return false;
    }

    // Do we already know the process is terminated?
    if (self->bTerminated) {
        return false;
    }

    // Let's check to make sure the process is still alive then
    // note: kill([pid], 0) doesn't send a signal but does perform error checking
    //       therefore, if it returns 0, the process is still alive, -1 means it errored out
    if (kill(self->ProcessId, 0)) {
        self->bTerminated = true;
        Log(error, "Target process is no longer alive");
        return false;
    }

    // Otherwise, keep going!
    return true;
}

//--------------------------------------------------------------------
//
// BeginMonitoring - Sync up monitoring threads 
//
//--------------------------------------------------------------------
bool BeginMonitoring(struct ProcDumpConfiguration *self)
{
    return SetEvent(&(self->evtStartMonitoring.event));
}

//--------------------------------------------------------------------
//
// IsValidNumberArg - quick helper function for ensuring arg is a number 
//
//--------------------------------------------------------------------
bool IsValidNumberArg(const char *arg)
{
    int strLen = strlen(arg);

    for (int i = 0; i < strLen; i++) {
        if (!isdigit(arg[i]) && !isspace(arg[i])) {
            return false;
        }
    }

    return true;
}


//--------------------------------------------------------------------
//
// PrintBanner - Not re-entrant safe banner printer. Function must be called before trigger threads start.
//
//--------------------------------------------------------------------
void PrintBanner()
{
    printf("\nProcDump v1.0.1 - Sysinternals process dump utility\n");
    printf("Copyright (C) 2017 Microsoft Corporation. All rights reserved. Licensed under the MIT license.\n");
    printf("Mark Russinovich, Mario Hewardt, John Salem, Javid Habibi\n");

    printf("Monitors a process and writes a dump file when the process exceeds the\n");
    printf("specified criteria.\n\n");
}


//--------------------------------------------------------------------
//
// PrintUsage - Print usage
//
//--------------------------------------------------------------------
int PrintUsage(struct ProcDumpConfiguration *self)
{
    printf("\nUsage: procdump [OPTIONS...] TARGET\n");
    printf("   OPTIONS\n");
    printf("      -h          Prints this help screen\n");
    printf("      -C          CPU threshold at which to create a dump of the process from 0 to 100 * nCPU\n");
    printf("      -c          CPU threshold below which to create a dump of the process from 0 to 100 * nCPU\n");
    printf("      -M          Memory commit threshold in MB at which to create a dump\n");
    printf("      -m          Trigger when memory commit drops below specified MB value.\n");
    printf("      -n          Number of dumps to write before exiting (default is %d)\n", DEFAULT_NUMBER_OF_DUMPS);
    printf("      -s          Consecutive seconds before dump is written (default is %d)\n", DEFAULT_DELTA_TIME);
    printf("      -d          Writes diagnostic logs to syslog\n");
    printf("   TARGET must be exactly one of these:\n");
    printf("      -p          pid of the process\n");
    printf("      -w          Name of the process executable\n\n");

    return -1;
}
