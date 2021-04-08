// make
// ./bin/osscheduler resrc/config_01.txt

#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    uint64_t current;
    uint64_t timeChange;
    bool terminatorCheck;
    Process::State process_state;
    std::list<int> priorities;
    int lowest_priority;

    while (!(shared_data->all_terminated))
    {
        // Clear output from previous iteration
        clearOutput(num_lines);

        // Do the following:
        //   - Get current time
        current = currentTime();
        timeChange = current - start;

        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        for(i = 0; i < processes.size(); i++)
        {
            process_state = processes[i]->getState();
            if(process_state == Process::State::NotStarted && timeChange >= processes[i]->getStartTime())
            {
                processes[i]->setState(Process::State::Ready, current);
                shared_data->mutex.lock();
                shared_data->ready_queue.push_back(processes[i]);
                shared_data->mutex.unlock();
            }
        }

        //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        for(i = 0; i < processes.size(); i++)
        {
            process_state = processes[i]->getState();
            if(process_state == Process::State::IO && current - processes[i]->getBurstStartTime() >= processes[i]->getBurstTime())
            {
                processes[i]->setState(Process::State::Ready, current);
                shared_data->mutex.lock();
                shared_data->ready_queue.push_back(processes[i]);
                shared_data->mutex.unlock();
            }
        }

        //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        if(shared_data->algorithm == ScheduleAlgorithm::PP)
        {
            shared_data->mutex.lock();
            for(i = 0; i < shared_data->ready_queue.size(); i++)
            {
                priorities.push_front(shared_data->ready_queue.front()->getPriority());
                shared_data->ready_queue.push_back(shared_data->ready_queue.front());
                shared_data->ready_queue.pop_front();
            }
            shared_data->mutex.unlock();
            priorities.sort();
            while(priorities.size() > num_cores)
            {
                priorities.pop_front();
            }
            lowest_priority = priorities.front();
            while(!priorities.empty())
            {
                priorities.pop_front();
            }
        }
        
        for(i = 0; i < processes.size(); i++)
        {
            process_state = processes[i]->getState();
            if(process_state == Process::State::Running && current - processes[i]->getBurstStartTime() >= shared_data->time_slice && shared_data->algorithm == ScheduleAlgorithm::RR)
            {
                processes[i]->interrupt();
                /*processes[i]->setState(Process::State::Ready, current);
                shared_data->mutex.lock();
                shared_data->ready_queue.push_back(processes[i]);
                shared_data->mutex.unlock();*/
            }
            if(process_state == Process::State::Running && processes[i]->getPriority() < lowest_priority && shared_data->algorithm == ScheduleAlgorithm::PP)
            {
                processes[i]->interrupt();
                /*processes[i]->setState(Process::State::Ready, current);
                shared_data->mutex.lock();
                shared_data->ready_queue.push_back(processes[i]);
                shared_data->mutex.unlock();*/
            }
        }

        //   - *Sort the ready queue (if needed - based on scheduling algorithm)
        shared_data->mutex.lock();
        if(shared_data->algorithm == ScheduleAlgorithm::SJF)
        {
            shared_data->ready_queue.sort(SjfComparator());
        }
        else if(shared_data->algorithm == ScheduleAlgorithm::PP)
        {
            shared_data->ready_queue.sort(PpComparator());
        }
        shared_data->mutex.unlock();

        //   - Determine if all processes are in the terminated state
        terminatorCheck = true;
        for(i = 0; i < processes.size(); i++)
        {
            process_state = processes[i]->getState();
            if(process_state != Process::State::Terminated)
            {
                terminatorCheck = false;
            }
        }
        shared_data->all_terminated = terminatorCheck;

        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000); // does not work on my (Peter) computer at all
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    float total_time = 0;
    float CPU_time = 0;
    float total_wait = 0;
    float CPU_util = 0;

    for(i = 0; i < processes.size(); i++)
    {
        total_time += processes[i]->getTurnaroundTime();
        CPU_time += processes[i]->getCpuTime();
        total_wait += processes[i]->getWaitTime();
    }

    CPU_util = CPU_time / currentTime() - start;

    //  - CPU utilization
    printf("CPU Utilization: %5.1f\n", CPU_util*100);

    //  - Throughput
    //     - Average for first 50% of processes finished
    //     - Average for second 50% of processes finished
    //     - Overall average
    printf("Average Throughput: 1 process per %5.1f miliseconds\n", total_time/processes.size());

    //  - Average turnaround time
    printf("Average turnabout time: %5.1f\n", total_time/processes.size());

    //  - Average waiting time
    printf("Average waiting time: %5.1f\n", total_wait/processes.size());


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core idependent of the other cores
    // Repeat until all processes in terminated state:
    while(!shared_data->all_terminated)
    {
    //   - *Get process at front of ready queue
        shared_data->mutex.lock();
        Process *processToRun = shared_data->ready_queue.front();
        shared_data->ready_queue.pop_front();
        shared_data->mutex.unlock();

    //   - Simulate the processes running until one of the following:
    //     - CPU burst time has elapsed
    //     - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
        processToRun->setCpuCore(core_id);
        while(processToRun->getState() == Process::State::Running && !processToRun->isInterrupted() && currentTime() - processToRun->getBurstStartTime() < processToRun->getBurstTime());
        processToRun->setCpuCore(-1);

    //  - Place the process back in the appropriate queue
    //     - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
    //     - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
    //     - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
        if(processToRun->isInterrupted())
        {
            shared_data->mutex.lock();
            shared_data->ready_queue.push_back(processToRun);
            processToRun->setState(Process::State::Ready, currentTime());
            shared_data->mutex.unlock();
        }
        else if(processToRun->getRemainingTime() > 0)
        {
            processToRun->setState(Process::State::IO, currentTime());
        }
        else if(processToRun->getRemainingTime() <= 0)
        {
            processToRun->setState(Process::State::Terminated, currentTime());
        }

    //  - Wait context switching time
    usleep(shared_data->context_switch);

    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    }
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
