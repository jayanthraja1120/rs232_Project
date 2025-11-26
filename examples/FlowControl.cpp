#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <mutex>
#include <cctype>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "CppLinuxSerial/SerialPort.hpp"

using namespace std;
using namespace std::chrono_literals;
using namespace mn::CppLinuxSerial;

// ---------------- Config ----------------
const string SERIAL_PORT = "/dev/ttyUSB0";
const BaudRate BAUD = BaudRate::B_115200;

const string SERVER_HOST = "192.168.50.2";
const int SERVER_PORT = 1024;
const int RETRY_DELAY = 3; // sec

const string HEADER = "STM:1:1::1";
const string FOOTER = ":";
const uint8_t STX = 0x02;
const uint8_t ETX = 0x03;

atomic<bool> running(true);
int sock_fd = -1;
mutex sock_mutex;

// ---------------- Utility Functions ----------------
string strip_leading_non_alnum(const string &s) {
    size_t i = 0;
    while (i < s.size() && !isalnum(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}

string escape_special_characters(const string &text) {
    string escaped;
    for (char c : text) {
        if (c == '\\') escaped += "\\\\";
        else if (c == ':') escaped += "\\:";
        else escaped += c;
    }
    return escaped;
}

// ---------------- TCP Functions ----------------
bool connect_to_server() {
    lock_guard<mutex> guard(sock_mutex);
    if (sock_fd != -1) {
        close(sock_fd);
        sock_fd = -1;
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[TCP] socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST.c_str(), &addr.sin_addr);

    cout << "[server] Connecting to " << SERVER_HOST << ":" << SERVER_PORT << "..." << endl;
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[TCP] connect");
        close(sock_fd);
        sock_fd = -1;
        return false;
    }

    cout << "[server] Connected." << endl;
    return true;
}

void send_server(const vector<uint8_t> &msg) {
    lock_guard<mutex> guard(sock_mutex);
    if (sock_fd < 0) {
        cout << "[TCP ] Socket not connected." << endl;
        return;
    }

    ssize_t sent = send(sock_fd, msg.data(), msg.size(), 0);
    if (sent < 0) {
        perror("[TCP] send");
        close(sock_fd);
        sock_fd = -1;
    } else {
        cout << "[TCP ] Sent to server: " << msg.data() << endl;
    }
}

// ---------------- Serial Reader Thread ----------------
void serial_reader() {
    SerialPort serialPort(SERIAL_PORT, BAUD,
                          NumDataBits::EIGHT, Parity::NONE, NumStopBits::ONE,
                          HardwareFlowControl::OFF, SoftwareFlowControl::OFF);

    serialPort.SetTimeout(100); // 100ms timeout

    try {
        serialPort.Open();
        cout << "[serial] Port opened successfully." << endl;
    } catch (exception &e) {
        cerr << "[serial] Error opening port: " << e.what() << endl;
        return;
    }

    string buffer;
    while (running) {
        try {
            string chunk;
            serialPort.Read(chunk);

            if (!chunk.empty()) {
                buffer += chunk;

                size_t pos;
                // Process each <CR> as end of a message
                while ((pos = buffer.find('\r')) != string::npos) {
                    string line = buffer.substr(0, pos);
                    buffer.erase(0, pos + 1);

                    if (!line.empty()) {
                        cout << "[serial] raw serial data: " << line << endl;

                        // --- Replicate Python behavior ---
                        string payload = strip_leading_non_alnum(line);
                        if (payload.size() > 1) payload = payload.substr(1);
                        cout << "[serial] modified serial data: " << payload << endl;

                        payload = escape_special_characters(payload);
                        string msg_str = HEADER + payload + FOOTER;
                        vector<uint8_t> msg;
                        msg.push_back(STX);
                        msg.insert(msg.end(), msg_str.begin(), msg_str.end());
                        msg.push_back(ETX);

                        send_server(msg);
                    }
                }
            }
        } catch (exception &ex) {
            cerr << "[serial] Exception: " << ex.what() << endl;
        }

        this_thread::sleep_for(50ms);
    }

    serialPort.Close();
    cout << "[serial] Port closed." << endl;
}

// ---------------- Main ----------------
int main() {
    cout << "[Main] Starting serial-to-TCP bridge..." << endl;

    thread serialThread(serial_reader);

    // Keep trying to connect to server
    while (running) {
        if (sock_fd < 0) {
            connect_to_server();
        }
        this_thread::sleep_for(chrono::seconds(RETRY_DELAY));
    }

    serialThread.join();
    if (sock_fd >= 0)
        close(sock_fd);

    cout << "Program exited cleanly." << endl;
    return 0;
}
