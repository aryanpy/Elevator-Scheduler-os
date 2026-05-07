#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

using namespace std;

struct Elevator {
    string bayID;
    int low;
    int high;
    int currentFloor;
    int capacity;
};

struct Person {
    string id;
    int start;
    int end;
};

struct Assignment {
    Person p;
    string elevID;
};

//global state for threads
vector<Elevator> elevators;
queue<Person> in_queue;
queue<Assignment> out_queue;

mutex in_mtx;
mutex out_mtx;

atomic<bool> keep_running(true);

//helper to handle API requests via raw sockets
string api_call(const string& method, const string& path, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        close(sock);
        return "";
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        //if connect fails, just retry later
        close(sock);
        return "";
    }

    string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), req.length(), 0); // // not handling partial sends

    char buf[4096]={0};
    string res = "";
    int bytes;
    //read full response
    while ((bytes = read(sock, buf, 4096)) > 0) {
        res.append(buf, bytes);
    }
    close(sock);

    auto pos = res.find("\r\n\r\n");
    return (pos != string::npos) ? res.substr(pos + 4) : res;
}


void fetch_input_thread(int port) { //thread 1 (Monitor /NextInput)
    while (keep_running) {
        string res = api_call("GET", "/NextInput", port);
        
        if (res == "NONE" || res.empty()) {
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }

        //parsing "P1 | 3 | 10"
        size_t p1 = res.find('|');
        size_t p2 = res.find('|', p1 + 1);
        
        if (p1 != string::npos && p2 != string::npos) {
            Person p;
            p.id = res.substr(0, p1);
            
            //trim space from ID if present
            while (!p.id.empty() && p.id.back() == ' ') p.id.pop_back();

            string s1 = res.substr(p1 + 1, p2 - p1 - 1);
            string s2 = res.substr(p2 + 1);

            //manual trim for floor numbers
            while (!s1.empty() && s1.front() == ' ') s1.erase(s1.begin());
            while (!s2.empty() && s2.front() == ' ') s2.erase(s2.begin());

            p.start = stoi(s1);
            p.end = stoi(s2);

            //lock queue before pushing
            lock_guard<mutex> lock(in_mtx);
            in_queue.push(p);
        }
    }
}


void schedule_logic_thread() { //thread 2 (Scheduling Logic)
    int last_assigned_idx = 0; 

    while (keep_running) {
        Person p;
        {
            lock_guard<mutex> lk(in_mtx);
            if (in_queue.empty()) {
                goto idle; 
            }
            p = in_queue.front();
            in_queue.pop();
        }

        {
            string picked = "";
            int num_elevators = elevators.size();
            
            
            for (int i = 0; i < num_elevators; ++i) {
                int check_idx = (last_assigned_idx + 1 + i) % num_elevators;
                auto& e = elevators[check_idx];
                
                if (p.start >= e.low && p.start <= e.high &&
                    p.end   >= e.low && p.end   <= e.high) {
                    picked = e.bayID;
                    last_assigned_idx = check_idx; 
                    break;
                }
            }

            if (!picked.empty()) {
                cout << "[Scheduler] Routing " << p.id << " to Elevator " << picked << endl;
                lock_guard<mutex> lk(out_mtx);
                out_queue.push({p, picked});
            }
        }
        continue;

        idle:
        this_thread::sleep_for(chrono::milliseconds(20));
    }
}


void push_output_thread(int port) {//thread 3 (monitor/addpersontoelevator)
    while (keep_running) {
        Assignment work;
        bool has_work = false;

        {
            //lock queue before accessing
            lock_guard<mutex> lock(out_mtx);
            if (!out_queue.empty()) {
                work = out_queue.front();
                out_queue.pop();
                has_work = true;
            }
        }

        if (has_work) {
            string path = "/AddPersonToElevator/" + work.p.id + "/" + work.elevID;
            api_call("PUT", path, port);
        } else {
            this_thread::sleep_for(chrono::milliseconds(20));
        }
    }
}

int main(int argc, char* argv[]) {
    //exact check for required arguments
    if (argc != 3) {
        cerr << "Usage: scheduler_os <bldg_file> <port>\n";
        return 1;
    }

    //load configuration
    ifstream f(argv[1]);
    if (!f.is_open()) {
        cerr << "Could not open file\n";
        return 1;
    }

    string line;
    while (getline(f, line)) {
        if (line.empty()) continue;
        stringstream ss(line);
        Elevator e;
        string tmp;
        getline(ss, e.bayID, '\t');
        getline(ss, tmp, '\t'); e.low = stoi(tmp);
        getline(ss, tmp, '\t'); e.high = stoi(tmp);
        getline(ss, tmp, '\t'); e.currentFloor = stoi(tmp);
        getline(ss, tmp, '\t'); e.capacity = stoi(tmp);
        elevators.push_back(e);
    }

    int port = stoi(argv[2]);

    //starting simulation
    string start_status = api_call("PUT", "/Simulation/start", port);
    if (start_status.empty()) {
        cerr << "Failed to connect to simulation\n";
        return 1;
    }
    cout << "Connected to simulation\n";

    //launch worker threads
    thread t1(fetch_input_thread, port);
    thread t2(schedule_logic_thread);
    thread t3(push_output_thread, port);

    cout << "Threads started\n";

    //main loop to check if simulation is done
    while (keep_running) {
        this_thread::sleep_for(chrono::seconds(1));
        string check = api_call("GET", "/Simulation/check", port);
        if (check.find("complete") != string::npos || check.find("stopped") != string::npos) {
            keep_running = false;
        }
    }

    t1.join();
    t2.join();
    t3.join();

    return 0;
}