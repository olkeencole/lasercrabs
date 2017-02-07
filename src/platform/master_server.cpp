#define _AMD64_

#include "game/master.h"

#include <thread>
#if _WIN32
#include <Windows.h>
#endif
#include <time.h>
#include <chrono>
#include "sock.h"
#include <unordered_map>
#include "asset/level.h"
#include "cjson/cJSON.h"

namespace VI
{

namespace platform
{

	u64 timestamp()
	{
		time_t t;
		::time(&t);
		return u64(t);
	}

	r64 time()
	{
		return r64(std::chrono::high_resolution_clock::now().time_since_epoch().count()) / 1000000000.0;
	}

	void sleep(r32 time)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(s64(time * 1000.0f)));
	}

}

namespace Net
{

namespace Master
{

#define MASTER_AUDIT_INTERVAL 1.25 // remove inactive nodes every x seconds
#define MASTER_MATCH_INTERVAL 0.5 // run matchmaking searches every x seconds
#define MASTER_INACTIVE_THRESHOLD 7.0 // remove node if it's inactive for x seconds
#define MASTER_SETTINGS_FILE "config.txt"

	namespace Settings
	{
		s32 secret;
	}

	struct Node // could be a server or client
	{
		enum class State
		{
			Invalid,
			ServerActive,
			ServerLoading,
			ServerIdle,
			ClientWaiting,
			ClientConnecting,
			ClientIdle,
			count,
		};

		r64 last_message_timestamp;
		State state;
		Sock::Address addr;
		ServerState server_state;
	};

	struct ClientConnection
	{
		Sock::Address client;
		Sock::Address server;
	};

	std::unordered_map<Sock::Address, Node> nodes;
	Sock::Handle sock;
	Messenger messenger;
	Array<Sock::Address> servers;
	Array<Sock::Address> clients_waiting;
	Array<ClientConnection> clients_connecting;
	Array<Sock::Address> servers_loading;

	Node* node_for_address(Sock::Address addr)
	{
		auto i = nodes.find(addr);
		Node* n;
		if (i == nodes.end())
		{
			auto i = nodes.insert(std::pair<Sock::Address, Node>(addr, Node()));
			n = &i.first->second;
			n->addr = addr;
		}
		else
			n = &i->second;
		return n;
	}

	void disconnected(Sock::Address addr)
	{
		const Node& node = *node_for_address(addr);
		if (node.state == Node::State::ServerActive
			|| node.state == Node::State::ServerLoading
			|| node.state == Node::State::ServerIdle)
		{
			// it's a server; remove from the server list
			for (s32 i = 0; i < servers.length; i++)
			{
				if (servers[i].equals(addr))
				{
					servers.remove(i);
					i--;
				}
			}

			// reset any clients trying to connect to this server
			for (s32 i = 0; i < clients_connecting.length; i++)
			{
				const ClientConnection& connection = clients_connecting[i];
				if (connection.server.equals(addr))
				{
					node_for_address(connection.client)->state = Node::State::ClientWaiting;
					clients_connecting.remove(i);
					i--;
				}
			}
		}
		else if (node.state == Node::State::ClientWaiting)
		{
			// it's a client waiting for a server; remove it from the wait list
			for (s32 i = 0; i < clients_waiting.length; i++)
			{
				if (clients_waiting[i].equals(addr))
				{
					clients_waiting.remove(i);
					i--;
				}
			}
		}
		else if (node.state == Node::State::ClientConnecting)
		{
			// remove it from the connecting list
			for (s32 i = 0; i < clients_connecting.length; i++)
			{
				if (clients_connecting[i].client.equals(addr))
				{
					clients_connecting.remove(i);
					i--;
				}
			}
		}
		nodes.erase(addr);
		messenger.remove(addr);
	}

	b8 send_server_load(r64 timestamp, Node* server, ServerState* s)
	{
		server->state = Node::State::ServerLoading;
		server->server_state = *s;
		using Stream = StreamWrite;
		StreamWrite p;
		packet_init(&p);
		messenger.add_header(&p, server->addr, Message::ServerLoad);
		if (!serialize_server_state(&p, s))
			return false;
		packet_finalize(&p);
		messenger.send(p, timestamp, server->addr, &sock);
		return true;
	}

	b8 send_client_connect(r64 timestamp, Sock::Address server_addr, Sock::Address addr)
	{
		using Stream = StreamWrite;
		StreamWrite p;
		packet_init(&p);
		messenger.add_header(&p, addr, Message::ClientConnect);
		serialize_u32(&p, server_addr.host);
		serialize_u16(&p, server_addr.port);
		packet_finalize(&p);
		messenger.send(p, timestamp, addr, &sock);
		return true;
	}

	b8 packet_handle(StreamRead* p, Sock::Address addr, r64 timestamp)
	{
		using Stream = StreamRead;
		SequenceID seq;
		serialize_int(p, SequenceID, seq, 0, NET_SEQUENCE_COUNT - 1);
		Message type;
		serialize_enum(p, Message, type);
		if (!messenger.received(type, seq, addr, &sock))
			return false; // out of order

		Node* node = node_for_address(addr);
		node->last_message_timestamp = timestamp;

		switch (type)
		{
			case Message::Ack:
			{
				break;
			}
			case Message::Disconnect:
			{
				disconnected(addr);
				break;
			}
			case Message::ClientRequestServer:
			{
				ServerState s;
				if (!serialize_server_state(p, &s))
					net_error();
				if (s.level < 0
					|| s.open_slots == 0
					|| s.level >= Asset::Level::count
					|| (s.story_mode && s.open_slots != 1)
					|| (s.story_mode && s.team_count != 2))
					net_error();
				if (node->state == Node::State::ClientConnecting)
				{
					// ignore
				}
				else if (node->state == Node::State::Invalid
					|| node->state == Node::State::ClientIdle
					|| node->state == Node::State::ClientWaiting)
				{
					node->server_state = s;
					if (node->state != Node::State::ClientWaiting)
					{
						// add to client waiting list
						clients_waiting.add(node->addr);
					}
					node->state = Node::State::ClientWaiting;
				}
				else // invalid state transition
					net_error();
				break;
			}
			case Message::ServerStatusUpdate:
			{
				s32 secret;
				serialize_s32(p, secret);
				if (secret != Settings::secret)
					net_error();
				b8 active;
				serialize_bool(p, active);
				ServerState s;
				if (!serialize_server_state(p, &s))
					net_error();
				if (node->state == Node::State::ServerLoading)
				{
					if (s.equals(node->server_state) && active) // done loading
					{
						node->server_state = s;
						node->state = Node::State::ServerActive;
						
						// tell clients to connect to this server
						for (s32 i = 0; i < clients_connecting.length; i++)
						{
							const ClientConnection& connection = clients_connecting[i];
							if (connection.server.equals(node->addr))
								send_client_connect(timestamp, node->addr, connection.client);
						}
					}
				}
				else if (node->state == Node::State::Invalid
					|| node->state == Node::State::ServerActive
					|| node->state == Node::State::ServerIdle)
				{
					node->server_state = s;
					if (node->state == Node::State::Invalid)
					{
						// add to server list
						servers.add(node->addr);
					}
					node->state = active ? Node::State::ServerActive : Node::State::ServerIdle;
				}
				break;
			}
			default:
			{
				net_error();
				break;
			}
		}

		return true;
	}

	Node* alloc_server(r64 timestamp, ServerState* s)
	{
		for (s32 i = 0; i < servers.length; i++)
		{
			Node* n = node_for_address(servers[i]);
			if (n->state == Node::State::ServerIdle)
			{
				send_server_load(timestamp, n, s);
				return n;
			}
		}
		return nullptr;
	}

	void client_queue_join(Node* server, Node* client)
	{
		vi_assert(client->state == Node::State::ClientWaiting);
		b8 found = false;
		for (s32 i = 0; i < clients_waiting.length; i++)
		{
			if (clients_waiting[i].equals(client->addr))
			{
				found = true;
				clients_waiting.remove(i);
				break;
			}
		}
		vi_assert(found);

		ClientConnection* connection = clients_connecting.add();
		connection->server = server->addr;
		connection->client = client->addr;

		client->state = Node::State::ClientConnecting;
	}

	s8 server_open_slots(Node* server)
	{
		if (server->state == Node::State::ServerLoading
			|| server->state == Node::State::ServerActive
			|| server->state == Node::State::ServerIdle)
		{
			s8 slots = server->server_state.open_slots;
			for (s32 i = 0; i < clients_connecting.length; i++)
			{
				const ClientConnection& connection = clients_connecting[i];
				if (connection.server.equals(server->addr))
					slots -= node_for_address(connection.client)->server_state.open_slots;
			}
			return slots;
		}
		else
			vi_assert(false);
	}

	s32 proc()
	{
		if (Sock::init())
			return 1;

		if (Sock::udp_open(&sock, 3497, true))
		{
			fprintf(stderr, "%s\n", Sock::get_error());
			return 1;
		}

		// load settings
		{
			FILE* f = fopen(MASTER_SETTINGS_FILE, "rb");
			if (f)
			{
				fseek(f, 0, SEEK_END);
				long len = ftell(f);
				fseek(f, 0, SEEK_SET);

				char* data = (char*)calloc(sizeof(char), len + 1);
				fread(data, 1, len, f);
				data[len] = '\0';
				fclose(f);

				cJSON* json = cJSON_Parse(data);
				if (json)
				{
					cJSON* secret = cJSON_GetObjectItem(json, "secret");
					if (secret)
						Settings::secret = secret->valueint;
					cJSON_Delete(json);
				}
				else
					fprintf(stderr, "Can't parse json file '%s': %s\n", MASTER_SETTINGS_FILE, cJSON_GetErrorPtr());
				free(data);
			}
		}

		r64 last_audit = 0.0;
		r64 last_match = 0.0;
		r64 timestamp = 0.0;
		r64 last_update = platform::time();

		while (true)
		{
			{
				r64 t = platform::time();
				timestamp += vi_min(0.25, t - last_update);
				last_update = t;
			}

			messenger.update(timestamp, &sock);

			// remove inactive nodes
			if (timestamp - last_audit > MASTER_AUDIT_INTERVAL)
			{
				r64 threshold = timestamp - MASTER_INACTIVE_THRESHOLD;
				Array<Sock::Address> removals;
				for (auto i = nodes.begin(); i != nodes.end(); i++)
				{
					if (i->second.last_message_timestamp < threshold)
						removals.add(i->first);
				}
				for (s32 i = 0; i < removals.length; i++)
					disconnected(removals[i]);
				last_audit = timestamp;
			}

			if (timestamp - last_match > MASTER_MATCH_INTERVAL)
			{
				last_match = timestamp;
				Array<Sock::Address> multiplayer_servers;
				s32 existing_multiplayer_slots = 0;

				s32 idle_servers = 0;
				for (s32 i = 0; i < servers.length; i++)
				{
					Node* server = node_for_address(servers[i]);
					if (server->state == Node::State::ServerIdle)
					{
						// find someone looking for a story-mode server and give this server to them
						for (s32 j = 0; j < clients_waiting.length; j++)
						{
							Node* client = node_for_address(clients_waiting[j]);
							if (client->server_state.story_mode)
							{
								send_server_load(timestamp, server, &client->server_state);
								client_queue_join(server, client);
								break;
							}
						}

						if (server->state == Node::State::ServerIdle) // still idle
							idle_servers += 1;
					}
					else
					{
						vi_assert(server->state == Node::State::ServerActive || server->state == Node::State::ServerLoading);
						s8 open_slots = server_open_slots(server);
						if (!server->server_state.story_mode && open_slots > 0)
						{
							multiplayer_servers.add(server->addr);
							existing_multiplayer_slots += open_slots;
						}
					}
				}

				// allocate multiplayer servers as necessary
				s32 needed_multiplayer_slots = 0;
				{
					for (s32 i = 0; i < clients_waiting.length; i++)
					{
						Node* node = node_for_address(clients_waiting[i]);
						if (!node->server_state.story_mode)
							needed_multiplayer_slots += node->server_state.open_slots;
					}

					// todo: different multiplayer setups
					ServerState multiplayer_state;
					multiplayer_state.level = Asset::Level::Medias_Res;
					multiplayer_state.open_slots = 4;
					multiplayer_state.story_mode = false;
					multiplayer_state.team_count = 2;

					s32 server_allocs = vi_min(idle_servers, ((needed_multiplayer_slots - existing_multiplayer_slots) + MAX_PLAYERS - 1) / MAX_PLAYERS); // ceil divide
					for (s32 i = 0; i < server_allocs; i++)
					{
						Node* server = alloc_server(timestamp, &multiplayer_state);
						if (server)
							multiplayer_servers.add(server->addr);
						else
							break; // not enough servers available
					}
				}

				// assign clients to servers
				for (s32 i = 0; i < multiplayer_servers.length; i++)
				{
					Node* server = node_for_address(multiplayer_servers[i]);
					s8 slots = server_open_slots(server);
					for (s32 j = 0; j < clients_waiting.length; j++)
					{
						Node* client = node_for_address(clients_waiting[j]);
						if (slots >= client->server_state.open_slots)
						{
							client_queue_join(server, client);
							if (server->state != Node::State::ServerLoading) // server already loaded, no need to wait for it
								send_client_connect(timestamp, server->addr, client->addr);
							slots -= client->server_state.open_slots;
							if (slots == 0)
								break;
						}
					}
				}
			}

			Sock::Address addr;
			StreamRead packet;
			s32 bytes_read = Sock::udp_receive(&sock, &addr, packet.data.data, NET_MAX_PACKET_SIZE);
			packet.resize_bytes(bytes_read);
			if (bytes_read > 0)
			{
				if (packet.read_checksum())
				{
					packet_decompress(&packet, bytes_read);
					packet_handle(&packet, addr, timestamp);
				}
				else
					vi_debug("%s", "Discarding packet due to invalid checksum.");
			}
			else
				platform::sleep(1.0f / 60.0f);
		}

		return 0;
	}

}

}

}

int main(int argc, char** argv)
{
	return VI::Net::Master::proc();
}