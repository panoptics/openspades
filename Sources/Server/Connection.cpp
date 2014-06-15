/*
 Copyright (c) 2013 yvt
 
 This file is part of OpenSpades.
 
 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 
 */

#include "Connection.h"
#include "Server.h"
#include <random>
#include "ServerEntity.h"
#include <Client/GameMap.h>
#include <Core/Thread.h>
#include <Core/Mutex.h>
#include <Core/AutoLocker.h>
#include <Core/IStream.h>
#include <Core/DeflateStream.h>
#include <thread>

namespace spades { namespace server {
	
	namespace {
		std::random_device randomDevice;
		std::string GenerateNonce(std::size_t len) {
			std::string s;
			s.resize(len);
			
			std::uniform_int_distribution<> u(0, 255);
			
			for (auto& c: s) {
				c = static_cast<char>(u(randomDevice));
			}
			
			return s;
		}
	}
	
	class Connection::MapGenerator: Thread {
	public:
		using Block = std::vector<char>;
	private:
		
		class AbortException: public std::exception { };
		
		void CheckAbort() {
			if (shouldAbort) {
				throw AbortException();
			}
		}
		
		void Push(Block&& block) {
			while (true) {
				std::size_t queueLen;
				{
					AutoLocker lock(&blocksMutex);
					queueLen = queuedBytes;
					CheckAbort();
				}
				if (queueLen > 1024 * 1024) {
					// queue too long
					std::this_thread::sleep_for
					(std::chrono::milliseconds(100));
				} else {
					break;
				}
			}
			
			AutoLocker lock(&blocksMutex);
			queuedBytes += block.size();
			blocks.emplace_back(std::move(block));
		}
		
		class Stream final: public IStream {
			MapGenerator& gen;
		public:
			Stream(MapGenerator& gen): gen(gen) { }
			void WriteByte(int b) override {
				auto c = static_cast<uint8_t>(b);
				Write(&c, 1);
			}
			void Write(const void *d, size_t bytes) override {
				
				Block block;
				block.resize(bytes);
				std::memcpy(block.data(), d, bytes);
				gen.Push(std::move(block));
			}
		};
		
		Handle<client::GameMap> map;
		std::list<Block> blocks;
		Mutex blocksMutex;
		std::size_t queuedBytes = 0;
		bool volatile done = false;
		bool volatile shouldAbort = false;
		
		void Run() override {
			try {
				Stream outputStream(*this);
				{
					client::NGMapOptions opt;
					opt.quality = 100;
					map->SaveNGMap(&outputStream, opt);
				}
			} catch (const AbortException&) {
				// aborted.
			}
			
			AutoLocker lock(&blocksMutex);
			done = true;
		}
	public:
		MapGenerator(const client::GameMap& map):
		// copy-on-write copy
		map(new client::GameMap(map), false) {
		}
		
		using Thread::MarkForAutoDeletion;
		
		void Start() {
			Thread::Start();
		}
		
		void Abort() {
			AutoLocker lock(&blocksMutex);
			shouldAbort = true;
		}
		
		bool SendAvailableBlock(Connection& c)
		{
			AutoLocker lock(&blocksMutex);
			std::array<char, 16384> fragment;
			if (queuedBytes - frontBlockPos >= fragment.size() ||
				done) {
				std::size_t outPos = 0;
				while (outPos < fragment.size()) {
					if (blocks.empty()) {
						break;
					}
					const auto& front = blocks.front();
					if (frontBlockPos == front.size()) {
						blocks.pop_front();
						frontBlockPos = 0;
					} else {
						auto copied = front.size() - frontBlockPos;
						copied = std::min(copied, fragment.size() - outPos);
						std::memcpy(fragment.data() + outPos,
									front.data() + frontBlockPos,
									copied);
						frontBlockPos += copied;
						outPos += copied;
					}
				}
				if (outPos > 0) {
					protocol::MapDataPacket p;
					p.fragment = std::string(fragment.data(), outPos);
					c.peer->Send(p);
					return false;
				} else {
					return done;
				}
			} else {
				return false;
			}
		}
	private:
		std::size_t frontBlockPos = 0;
	};
	
	Connection::Connection(Server *server):
	server(server),
	peer(nullptr) {
		
	}
	
	game::World& Connection::GetWorld() {
		return server->GetWorld();
	}
	
	void Connection::Initialize(HostPeer *peer) {
		// TODO: do something
		
		peer->SetListener(static_cast<HostPeerListener *>(this));
		this->peer = peer;
		
		protocol::GreetingPacket p;
		serverNonce = GenerateNonce(128);
		clientNonce = serverNonce;
		nonce = serverNonce + clientNonce;
		p.nonce = serverNonce;
		peer->Send(p);
		
		SPLog("%s sent GreetingPacket",
			  peer->GetLogHeader().c_str());
		
		state = State::NotInitiated;
		stateTimeout = 10.0;
		
		mapGenerator = nullptr;
	}
	
	Connection::~Connection() {
		if (mapGenerator) {
			mapGenerator->Abort();
			mapGenerator->MarkForAutoDeletion();
			mapGenerator = nullptr;
		}
		
		auto& conns = server->connections;
		auto it = conns.find(this);
		if(it != conns.end())
			conns.erase(it);
	}
	
	void Connection::OnWorldChanged() {
		if (!peer) return;
		StartStateTransfer();
	}
	
	void Connection::Update(double dt) {
		if (!peer) return;
		
		if (state == State::MapTransfer) {
			SPAssert(mapGenerator);
			if (peer->GetPendingBytes() < 64 * 1024 &&
				mapGenerator->SendAvailableBlock(*this)) {
				// complete.
				state = State::CompletingMapTransfer;
				protocol::MapDataFinalPacket p;
				peer->Send(p);
				SPLog("%s sent CompletingMapTransfer",
					  peer->GetLogHeader().c_str());
			}
		}
		
		if (state != State::Game) {
			stateTimeout -= dt;
			if (stateTimeout < 0.0) {
				// client should have responded earlier.
				SPLog("%s timed out",
					  peer->GetLogHeader().c_str());
				peer->Disconnect(protocol::DisconnectReason::Timeout);
				peer = nullptr;
			}
			return;
		}
		
	}
	
	void Connection::Disconnected() {
		peer = nullptr;
		// HostPeer will release the reference to this
		// instance after calling this function.
	}
	
	class Connection::PacketVisitor:
	public protocol::ConstPacketVisitor {
		Connection& c;
	public:
		
		PacketVisitor(Connection& c): c(c) { }
		void visit(const protocol::GreetingPacket& p) override {
			SPRaise("Unexpected GreetingPacket.");
		}
		void visit(const protocol::InitiateConnectionPacket& p) override {
			SPLog("%s [InitiateConnectionPacket]",
				  c.peer->GetLogHeader().c_str());
			if (c.state == State::NotInitiated) {
				if (p.protocolName != protocol::ProtocolName) {
					c.peer->Disconnect(protocol::DisconnectReason::ProtocolMismatch);
					c.peer = nullptr;
					return;
				}
				SPLog("%s protocol = '%s'",
					  c.peer->GetLogHeader().c_str(),
					  p.protocolName.c_str());
				SPLog("%s version = %d.%d.%d",
					  c.peer->GetLogHeader().c_str(),
					  (int)p.majorVersion,
					  (int)p.minorVersion,
					  (int)p.revision);
				SPLog("%s package = '%s'",
					  c.peer->GetLogHeader().c_str(),
					  p.packageString.c_str());
				SPLog("%s environment = '%s'",
					  c.peer->GetLogHeader().c_str(),
					  p.environmentString.c_str());
				SPLog("%s locale = '%s'",
					  c.peer->GetLogHeader().c_str(),
					  p.locale.c_str());
				SPLog("%s name = '%s'",
					  c.peer->GetLogHeader().c_str(),
					  p.playerName.c_str());
				SPLog("%s nonce = %d byte(s)",
					  c.peer->GetLogHeader().c_str(),
					  (int)p.nonce.size());
				c.clientNonce = p.nonce;
				c.nonce = c.serverNonce + c.clientNonce;
				c.SendServerCertificate();
			} else {
				SPRaise("Unexpected InitiateConnectionPacket.");
			}
		}
		void visit(const protocol::ServerCertificatePacket& p) override {
			SPRaise("Unexpected ServerCertificatePacket.");
		}
		void visit(const protocol::ClientCertificatePacket& p) override {
			SPLog("%s [ClientCertificatePacket]",
				  c.peer->GetLogHeader().c_str());
			if (c.state == State::WaitingForCertificate) {
				// TODO: validate certificate
				if (p.isValid) {
					SPLog("%s %d byte(s) of cert and %d byte(s) of sig",
						  c.peer->GetLogHeader().c_str(),
						  (int)p.certificate.size(),
						  (int)p.signature.size());
				} else {
					SPLog("%s no certificate.",
						  c.peer->GetLogHeader().c_str());
				}
				c.StartStateTransfer();
			} else {
				SPRaise("Unexpected ClientCertificatePacket.");
			}
		}
		void visit(const protocol::KickPacket& p) override {
			SPRaise("Unexpected KickPacket.");
		}
		void visit(const protocol::GameStateHeaderPacket& p) override {
			SPRaise("Unexpected GameStateHeaderPacket.");
		}
		void visit(const protocol::MapDataPacket& p) override {
			SPRaise("Unexpected MapDataPacket.");
		}
		void visit(const protocol::MapDataFinalPacket& p) override {
			SPRaise("Unexpected MapDataFinalPacket.");
		}
		void visit(const protocol::MapDataAcknowledgePacket& p) override {
			SPLog("%s [MapDataAcknowledgePacket]",
				  c.peer->GetLogHeader().c_str());
			if (c.state == State::CompletingMapTransfer) {
				c.FinalStateTransfer();
			} else {
				SPRaise("Unexpected MapDataAcknowledgePacket.");
			}
		}
		void visit(const protocol::GameStateFinalPacket& p) override {
			SPRaise("Unexpected GameStateFinalPacket.");
		}
		void visit(const protocol::GenericCommandPacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::EntityUpdatePacket& p) override {
			SPRaise("Unexpected EntityUpdatePacket.");
		}
		void visit(const protocol::ClientSideEntityUpdatePacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::TerrainUpdatePacket& p) override {
			SPRaise("Unexpected TerrainUpdatePacket.");
		}
		void visit(const protocol::EntityEventPacket& p) override {
			SPRaise("Unexpected EntityEventPacket.");
		}
		void visit(const protocol::EntityDiePacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::EntityRemovePacket& p) override {
			SPRaise("Unexpected EntityRemovePacket.");
		}
		void visit(const protocol::PlayerActionPacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::HitEntityPacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::HitTerrainPacket& p) override {
			SPNotImplemented();
		}
		void visit(const protocol::DamagePacket& p) override {
			SPRaise("Unexpected DamagePacket.");
		}
		
	};
	
	void Connection::PacketReceived(const protocol::Packet &packet) {
		if (!peer) return;
		
		try {
			PacketVisitor visitor(*this);
			packet.Accept(visitor);
		} catch (const std::exception& ex) {
			SPLog("%s peer was kicked because an error occured while handling packet: %s",
				  peer->GetLogHeader().c_str(),
				  ex.what());
			peer->Disconnect(protocol::DisconnectReason::MalformedPacket);
			peer = nullptr;
		}
	}
	
	void Connection::SendServerCertificate() {
		protocol::ServerCertificatePacket re;
		// TOOD: server certificate
		re.isValid = false;
		peer->Send(re);
		
		SPLog("%s sent ServerCertificatePacket",
			  peer->GetLogHeader().c_str());
		
		state = State::WaitingForCertificate;
		stateTimeout = 60.0;
	}
	
	void Connection::StartStateTransfer() {
		if (!peer) return;
		
		state = State::MapTransfer;
		stateTimeout = 300.0;
		
		protocol::GameStateHeaderPacket re;
		peer->Send(re);
		
		SPLog("%s sent GameStateHeaderPacket",
			  peer->GetLogHeader().c_str());
		
		if (mapGenerator) {
			mapGenerator->Abort();
			mapGenerator->MarkForAutoDeletion();
			mapGenerator = nullptr;
		}
		
		mapGenerator = new MapGenerator(*GetWorld().GetGameMap());
		mapGenerator->Start();
	}
	
	void Connection::FinalStateTransfer() {
		if (!peer) return;
		
		protocol::GameStateFinalPacket re;
		
		re.properties = GetWorld().GetParameters().Serialize();
		re.items.reserve(server->serverEntities.size());
		for (const auto& e: server->serverEntities) {
			re.items.push_back(e->Serialize());
		}
		peer->Send(re);
		
		SPLog("%s sent GameStateFinalPacket",
			  peer->GetLogHeader().c_str());
		
		// welcome to the server!
		state = State::Game;
	}
	
	
} }
