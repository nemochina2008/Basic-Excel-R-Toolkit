
#include "controlr.hpp"
#include "pipe.hpp"
#include "util.hpp"

#include "controlr_rinterface.h"
#include "variable.pb.h"

#include "console_message.h"

#define MESSAGE_OVERHEAD 64

// pipe index of callback
#define CALLBACK_INDEX          0

// pipe index of primary client
#define PRIMARY_CLIENT_INDEX    1

// #include <google\protobuf\util\json_util.h> // dev

extern BERTBuffers::Response& RCall(BERTBuffers::Response &rsp, const BERTBuffers::FunctionCall &function_call, bool wait);
extern BERTBuffers::Response& RExec(BERTBuffers::Response &rsp, const BERTBuffers::Call &call);

std::string pipename;
std::string rhome;

//HANDLE console_out_event = ::CreateEvent(0, TRUE, FALSE, 0);
int console_client = -1;

std::vector<Pipe*> pipes;
std::vector<HANDLE> handles; //  = { console_out_event };
std::vector<std::string> console_buffer;

// flag indicates we are operating after a break; changes prompt semantics
bool user_break_flag = false;

std::string GetLastErrorAsString(DWORD err = -1)
{
	//Get the error message, if any.
	DWORD errorMessageID = err;
	if( -1 == err ) errorMessageID = ::GetLastError();
	if (errorMessageID == 0)
		return std::string(); //No error message has been recorded

	LPSTR messageBuffer = nullptr;
	size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

	std::string message(messageBuffer, size);

	//Free the buffer.
	LocalFree(messageBuffer);

	return message;
}

void DirectCallback(const char *channel, const char *data, bool buffered) {

}

bool UnframeMessage(google::protobuf::Message &message, const std::string &str) {
    int32_t bytes;
    const char *data = str.c_str();
    memcpy(reinterpret_cast<void*>(&bytes), data, sizeof(int32_t));
    return message.ParseFromArray(data + sizeof(int32_t), bytes);
}

std::string FrameMessage(const google::protobuf::Message &message) {
    int32_t bytes = message.ByteSize();
    std::stringstream stream;
    stream.write(reinterpret_cast<char*>(&bytes), sizeof(int32_t));
    message.SerializeToOstream(dynamic_cast<std::ostream*>(&stream));
    return stream.str();
}

void ConsoleResetPrompt(uint32_t id) {
    BERTBuffers::Response rsp;
    rsp.set_id(id);
    //rsp.mutable_console()->set_reset_prompt(true);
    //rsp.mutable_console()->set_control_message("reset-prompt");
    rsp.set_control_message("reset-prompt");
    std::string framed = FrameMessage(rsp);
    if (console_client >= 0) {
        pipes[console_client]->PushWrite(framed);
    }
    else {
        console_buffer.push_back(framed);
    }
}

void ConsolePrompt(const char *prompt, uint32_t id) {

    BERTBuffers::Response rsp;
    rsp.set_id(id);
    rsp.mutable_console()->set_prompt(prompt);
    std::string framed = FrameMessage(rsp);

    if (console_client >= 0) {
        pipes[console_client]->PushWrite(framed);
    }
    else {
        console_buffer.push_back(framed);
    }

}

bool Callback(const BERTBuffers::Call &call, BERTBuffers::Response &response){
    auto pipe = pipes[CALLBACK_INDEX];
    if (!pipe->connected()) return false;

    pipe->PushWrite(FrameMessage(call));
    pipe->StartRead(); // probably not necessary

    std::string data;
    DWORD result;
    do {
        result = pipe->Read(data, true);
    } while (result == ERROR_MORE_DATA);

    if (!result) UnframeMessage(response, data);

    pipe->StartRead(); // probably not necessary either
    return (result == 0);
}

void ConsoleControlMessage(const std::string &message) {
    BERTBuffers::Response response;
    //response.mutable_console()->set_control_message(message);
    response.set_control_message(message);
    std::string framed = FrameMessage(response);
    if (console_client >= 0) pipes[console_client]->PushWrite(framed);
    else console_buffer.push_back(framed);
}

void ConsoleMessage(const char *buf, int len, int flag) {

	//string message(buf, len);
	//cout << message;

    BERTBuffers::Response rsp;
    if (flag) rsp.mutable_console()->set_err(buf);
    else rsp.mutable_console()->set_text(buf);

    std::string framed = FrameMessage(rsp);

    if (console_client >= 0) {
        pipes[console_client]->PushWrite(framed);
    }
    else {
        console_buffer.push_back(framed);
    }

}

void Shutdown(int exit_code) {
	ExitProcess(0);
}

void NextPipeInstance(bool block, std::string &name) {
	Pipe *pipe = new Pipe;
	int rslt = pipe->Start(name, block);
	handles.push_back(pipe->wait_handle_read());
	handles.push_back(pipe->wait_handle_write());
	pipes.push_back(pipe);
}

void CloseClient(int index) {

    // shutdown if primary client breaks connection
    if (index == PRIMARY_CLIENT_INDEX) Shutdown(-1);

    // callback shouldn't close either
    else if (index == CALLBACK_INDEX) {
        cerr << "callback pipe closed" << endl;
        // Shutdown(-1);
    }

    // otherwise clean up, and watch out for console
    else {
        pipes[index]->Reset();
        if (index == console_client) {
            console_client = -1;
        }
    }

}

int InputStreamRead(const char *prompt, char *buf, int len, int addtohistory, bool is_continuation) {

    // it turns out this function can get called recursively. we
    // hijack this function to run non-interactive R calls, but if
    // one of those calls wants a shell interface (such as a debug
    // browser, it will call into this function again). this gets
    // a little hard to track on the UI side, as we have extra prompts
    // from the internal calls, but we don't know when those are 
    // finished.

    // however we should be able to figure this out just by tracking
    // recursion. note that this is never threaded.

    static uint32_t call_depth = 0;
    static bool recursive_calls = false;

    static uint32_t prompt_transaction_id = 0;

	std::string buffer;
	std::string message;

	DWORD result;
    
    if (call_depth > 0) {
        // set flag to indicate we'll need to "unwind" the console
        cout << "console prompt at depth " << call_depth << endl;
        recursive_calls = true; 
    }

    ConsolePrompt(prompt, prompt_transaction_id);

	while (true) {

		result = WaitForMultipleObjects(handles.size(), &(handles[0]), FALSE, 100);

		if(result >= WAIT_OBJECT_0 && result - WAIT_OBJECT_0 < 16){

			int offset = (result - WAIT_OBJECT_0);
			int index = offset / 2;
			bool write = offset % 2;
            auto pipe = pipes[index];

            if (!index) cout << "pipe event on index 0 (" << (write ? "write" : "read") << ")" << endl;

			ResetEvent(handles[result - WAIT_OBJECT_0]);

			if (!pipe->connected()){
                cout << "connect (" << index << ")" << endl;
				pipe->Connect(); // this will start reading
                if( pipes.size() < MAX_PIPE_COUNT ) NextPipeInstance(false, pipename);
			}
			else if (write) {
                pipe->NextWrite();
			}
			else {
				result = pipe->Read(message);

				if (!result) {

					BERTBuffers::Call call;
					BERTBuffers::Response rsp;
					//bool success = call.ParseFromString(message);
                    bool success = UnframeMessage(call, message);

					if (success) {

						rsp.set_id(call.id());
						switch( call.call_case()){

						case BERTBuffers::Call::kFunctionCall:
                            call_depth++;
							RCall(rsp, call.function_call(), call.wait());
                            call_depth--;
							//if (call.wait()) pipe->PushWrite(rsp.SerializeAsString());
                            if (call.wait()) pipe->PushWrite(FrameMessage(rsp));
							break;

						case BERTBuffers::Call::kCode:
                            call_depth++;
                            RExec(rsp, call);
                            call_depth--;
							//if (call.wait()) pipe->PushWrite(rsp.SerializeAsString());
                            if (call.wait()) pipe->PushWrite(FrameMessage(rsp));
                            break;

						case BERTBuffers::Call::kShellCommand:
							len = min(len - 2, call.shell_command().length());
							strcpy_s(buf, len + 1, call.shell_command().c_str());
							buf[len++] = '\n';
							buf[len++] = 0;

							//if (call.wait()) {
							//	rsp.set_id(call.id());
							//	//pipe->PushWrite(rsp.SerializeAsString());
                            //    pipe->PushWrite(FrameResponse(rsp));
                            //}

                            prompt_transaction_id = call.id();
							pipe->StartRead(); 

							// start read and then exit this function; that will cycle the R REPL loop.
                            // the (implicit/explicit) response from this command is going to be the next 
                            // prompt.

							return len;

						case BERTBuffers::Call::kSystemCommand:
						{
							std::string command = call.system_command();
							cout << "system command: " << command << endl;
							if (!command.compare("shutdown")) {
                                ConsoleControlMessage("shutdown");
								Shutdown(0);
							}
							else if (!command.compare("console")) {
								if (console_client < 0) {
									console_client = index;
									cout << "set console client -> " << index << endl;
                                    pipe->QueueWrites(console_buffer);
                                    console_buffer.clear();
								}
								else cerr << "console client already set" << endl;
							}
                            else if (!command.compare("close")) {
                                CloseClient(index);
                                break; // no response 
                            }
							else {
								// ...
							}

							if (call.wait()) {
								rsp.set_id(call.id());
								//pipe->PushWrite(rsp.SerializeAsString());
                                pipe->PushWrite(FrameMessage(rsp));
							}
                            else pipe->NextWrite();
						}
						break;

						default:
							// ...
							0;
						}

                        if (call_depth == 0 && recursive_calls) {
                            cout << "unwind recursive prompt stack" << endl;
                            recursive_calls = false;
                            ConsoleResetPrompt(prompt_transaction_id);
                        }

					}
					else {
						if (pipe->error()) {
							cout << "ERR in system pipe: " << result << endl;
						}
                        else cerr << "error parsing packet: " << result << endl;
                    }
					if (pipe->connected() && !pipe->reading()){
						pipe->StartRead();
					}
				}
				else {
					if (result == ERROR_BROKEN_PIPE) {
						cerr << "broken pipe (" << index << ")" << endl;
                        CloseClient(index);
					}
                    //else if (result == ERROR_MORE_DATA) {
                    //    cout << "(more data...)" << endl;
                    //}
				}
			}
		}
		else if (result == WAIT_TIMEOUT) {
            RTick();
		}
		else {
			cerr << "ERR " << result << ": " << GetLastErrorAsString(result) << endl;
			break;
		}
	}

	return 0;
}

unsigned __stdcall ManagementThreadFunction(void *data) {

    DWORD result;
    Pipe pipe;
    char *name = reinterpret_cast<char*>(data);

    cout << "start management pipe on " << name << endl;

    int rslt = pipe.Start(name, false);
    std::string message;
    
    while (true) {
        result = WaitForSingleObject(pipe.wait_handle_read(), 1000);
        if (result == WAIT_OBJECT_0) {
            ResetEvent(pipe.wait_handle_read());
            if (!pipe.connected()) {
                cout << "connect management pipe" << endl;
                pipe.Connect(); // this will start reading
            }
            else {
                result = pipe.Read(message);
                if (!result) {
                    BERTBuffers::Call call;
                    bool success = UnframeMessage(call, message);
                    if (success) {
                        std::string command = call.system_command();
                        if (command.length()) {
                            if (!command.compare("break")) {
                                user_break_flag = true;
                                RSetUserBreak();
                            }
                            else {
                                cerr << "unexpected system command (management pipe): " << command << endl;
                            }
                        }
                    }
                    else {
                        cerr << "error parsing management message" << endl;
                    }
                    pipe.StartRead();
                }
                else {
                    if (result == ERROR_BROKEN_PIPE) {
                        cerr << "broken pipe in management thread" << endl;
                        pipe.Reset();
                    }
                }
            }
        }
        else if (result != WAIT_TIMEOUT) {
            cerr << "error in management thread: " << GetLastError() << endl;
            pipe.Reset();
            break;
        }
    }
    return 0;
}

/*
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        cout << "ctrl+C" << endl;
    }
    else {
        cout << "other signal " << signal << endl;
    }
    return TRUE;
}
*/

int main(int argc, char** argv) {
	
	for (int i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "-p", 2) && i < argc - 1) {
			pipename = argv[++i];
		}
		else if (!strncmp(argv[i], "-r", 2) && i < argc - 1) {
			rhome = argv[++i];
		}
	}

	if (!pipename.length()) {
		cerr << "call with -p pipename" << endl;
		return -1;
	}
	if (!rhome.length()) {
		cerr << "call with -r rhome" << endl;
		return -1;
	}

    cout << "pipe: " << pipename << endl;
    cout << "pid: " << _getpid() << endl;

    // we need a non-const block for the thread function. 
    // it just gets used once, and immediately

    char buffer[MAX_PATH];
    sprintf_s(buffer, "%s-M", pipename.c_str());
    uintptr_t thread_handle = _beginthreadex(0, 0, ManagementThreadFunction, buffer, 0, 0);

    // start the callback pipe first. doesn't block.

    std::string callback_pipe_name = pipename;
    callback_pipe_name += "-CB";
    NextPipeInstance(false, callback_pipe_name);

	// the first connection blocks. we don't start R until there's a client.

	NextPipeInstance(true, pipename);

	// start next pipe listener, don't block

	NextPipeInstance(false, pipename);

	// now start R 

	char no_save[] = "--no-save";
	char no_restore[] = "--no-restore";
	char* args[] = { argv[0], no_save, no_restore };

	int result = RLoop(rhome.c_str(), "", 3, args);
	if (result) cerr << "R loop failed: " << result << endl;

	handles.clear();
	for (auto pipe : pipes) delete pipe;

	pipes.clear();

	return 0;
}
