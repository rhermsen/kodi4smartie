
#include "stdafx.h"
#include <cpprest/ws_client.h>
#include <cpprest/json.h>
#include <cpprest/rawptrstream.h>
#include <Windows.h>
#include <ppltasks.h>
#include <iostream>
#include <string>
#include <conio.h>
#include <condition_variable>
#include <atomic>
#include <mutex>

#include "interface.hpp"
#include "logging.hpp"
#include "config.hpp"

using namespace web;
using namespace web::websockets::client;
using namespace Concurrency::streams;
using namespace concurrency;
using namespace std;

typedef struct
{
	json::value val;
	CONDITION_VARIABLE cv;
	CRITICAL_SECTION   lk;
} message_t;

std::condition_variable cv;
std::unique_lock<std::mutex> m;
websocket_callback_client *cbclient;
bool connected = false;
typedef map<uint32_t, message_t *> message_map_t;
message_map_t message_map;
atomic<uint32_t> serial = ATOMIC_VAR_INIT(10);
std::mutex map_mtx;


//debug routine
__declspec(dllexport) void display(json::value val, utility::string_t offset)
{
	for (auto it = val.as_object().begin(); it != val.as_object().end(); it++)
	{
		if (it->second.is_array())
		{
			wcout << offset << it->first << endl;
			display(it->second[0], offset + utility::string_t(U("  ")));
		}
		else
		{
			wcout << offset << it->first << " = " << it->second << endl;
		}
	}
}


__declspec(dllexport) void ws_connect()
{
	utility::string_t host = utility::string_t(U("ws://")) +
		utility::conversions::to_string_t(get_config_str(cHOST)) +
		utility::string_t(U(":")) +
		utility::conversions::to_string_t(get_config_str(cPORT));

	::log("Connecting to %ls", host.c_str());
	try
	{
		cbclient = new websocket_callback_client();
		cbclient->connect(host).then([host](pplx::task<void> t)
		{ 	
			t.get();
			connected = true;/* We've finished connecting. */
			::log("Connected to %ls", host.c_str());
			start_listen();
	
		}).wait();
	}
	catch (const websocket_exception &e) 
	{
		::log("Error connecting. Error= %s", e.what());
		delete cbclient;
		cbclient = NULL;
	};

}

__declspec(dllexport) void ws_close()
{
	if (connected == true)
	{ 
		connected = false;
		cbclient->close().wait();
		delete cbclient;
		cbclient = NULL;
	}
}

__declspec(dllexport) void start_listen()
{

	cbclient->set_close_handler([&](websocket_close_status close_status, const utility::string_t reason, const std::error_code& error) {
		log("Client Close Detected: status=%d", close_status);
		connected = false;
	});


	cbclient->set_message_handler([&](websocket_incoming_message msg) {
		message_map_t::iterator it;
		json::value val;

		utility::string_t instring = utility::string_t(utility::conversions::to_string_t(msg.extract_string().get()));
		json::value in = json::value::parse(instring);
		log("Received: %ls", instring.c_str());

		//see if this message was a reply
		val = in[U("id")];
		map_mtx.lock();
		if (!val.is_null() && (it = message_map.find(val.as_integer())) != message_map.end())
		{
			//tell who was waiting for this message that it is ready to go.
			it->second->val = in;
			map_mtx.unlock();
			WakeConditionVariable(&it->second->cv);
		}
		else
		{
			map_mtx.unlock();
			//we got an unrequested message (notification). parse it as see what we should do with it.
			parse(in);
		}
	});
}

// send a message and wait for a response.
__declspec(dllexport) json::value ws_send_wait(json::value input)
{
	websocket_outgoing_message msg;
	json::value result;
	message_map_t::iterator it;
	message_t *message = NULL;
	uint32_t my_serial;

	//add the standard stuff
	input[U("jsonrpc")] = json::value(U("2.0"));

	do
	{
		my_serial = std::atomic_fetch_add(&serial, 1);
		input[U("id")] = json::value(my_serial);

		//create the container to hold the results
		message = new message_t();

		//init the conditional variable so we wait for an answer to this request
		InitializeConditionVariable(&message->cv);

		// init the critical section variable
		InitializeCriticalSection(&message->lk);

		//add our message onto the map
		map_mtx.lock();
		message_map.insert(pair<uint32_t, message_t *>(my_serial, message));
		map_mtx.unlock();
		msg.set_utf8_message(utility::conversions::utf16_to_utf8(input.serialize()));

		log("Sending: %ls", input.serialize().c_str());

		EnterCriticalSection(&message->lk);

		cbclient->send(msg).then([](pplx::task<void> t)
		{
			try
			{
				t.get();
			}
			catch (const websocket_exception &e)
			{
				::log("Error sending. Error= %s", e.what());
			}

		}).wait();

		//wait for the response
		SleepConditionVariableCS(&message->cv, &message->lk, 5000);
		LeaveCriticalSection(&message->lk);

		map_mtx.lock();
		//get the reply
		if (message->val.size() > 0)
		{
			break;
		}
		//mesage timed out so clean up and resend. Not sure we need this.
		log("Retrying previous message");
		it = message_map.find(my_serial);
		message_map.erase(it);
		delete message;
		map_mtx.unlock();
	} while (true);

	result = message->val;
	//clean up from this message
	it = message_map.find(my_serial);
	message_map.erase(it);
	delete message;
	map_mtx.unlock();

	return result;
}

__declspec(dllexport) json::value ws_send_wait(utility::string_t method, json::value input)
{
	input[U("method")] = json::value(method);
	
	return ws_send_wait(input);
}
