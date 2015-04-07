/*
 Copyright (c) 2013 yvt
 based on code of pysnip (c) Mathias Kaerlev 2011-2012.
 
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

#pragma once

#include <stdint.h>
#include "../Core/Debug.h"
#include "../Core/Math.h"

#include "IGameMapListener.h"
#include <Core/RefCountedObject.h>
#include <list>
#include <Core/Mutex.h>
#include <Core/AutoLocker.h>

namespace spades{
	class IStream;
	namespace client {
		class GameMap: public RefCountedObject {
		protected:
			~GameMap();
		public:
			// fixed for now
			enum {
				DefaultWidth = 1024,
				DefaultHeight = 1024,
				DefaultDepth = 64 // should be <= 64

			};
			GameMap();
			
			static GameMap *Load(IStream *);
			
			void Save(IStream *);
			
			inline int Width() { return DefaultWidth; }
			inline int Height() { return DefaultHeight; }
			inline int Depth() { return DefaultDepth; }
			inline int GroundDepth() { return DefaultDepth-2; }
			inline int WaterDepth() { return DefaultDepth-7;  }//DefaultDepth-1;
			inline float GetCullDistance(void) { return cullDistance; }
			inline void SetCullDistance(float d) { cullDistance =d; }

			inline uint64_t QuantizeZ(uint64_t z) {
				return z/64;
			}

			inline bool IsSolid(int x, int y, int z) {
				SPAssert(x >= 0); SPAssert(x < Width());
				SPAssert(y >= 0); SPAssert(y < Height());
				SPAssert(z >= 0); SPAssert(z < Depth());
				return (solidMap[QuantizeZ(z)][x][y] >> ( (uint64_t)z - QuantizeZ(z)*64 ) & 1ULL) != 0;
			}
			
			/** @return 0xHHBBGGRR where HH is health (up to 100) */
			inline uint32_t GetColor(int x, int y, int z){
				SPAssert(x >= 0); SPAssert(x < Width() );
				SPAssert(y >= 0); SPAssert(y < Height() );
				SPAssert(z >= 0); SPAssert(z < Depth()+1 );
				return colorMap[x][y][z];
			}
			
			inline uint64_t GetSolidMapWrapped(int x, int y) {
				return solidMap[0][x & (Width() - 1)][y & (Height() - 1)];
			}
			
			inline bool IsSolidWrapped(int x, int y, int z){
				if(z < 0)
					return false;
				if(z >= Depth())
					return true;
				return (solidMap[QuantizeZ(z)][x & (Width() - 1)][y & (Height() - 1)] >> ( (uint64_t)z - QuantizeZ(z)*64 ) & 1ULL) != 0;
			}
			
			inline uint32_t GetColorWrapped(int x, int y, int z){
				return colorMap[x & (Width() - 1)]
							   [y & (Height() - 1)]
							   [z & (Depth() - 1)];
			}
			
			inline void Set(int x, int y, int z, bool solid, uint32_t color, bool unsafe = false){
				SPAssert(x >= 0); SPAssert(x < Width());
				SPAssert(y >= 0); SPAssert(y < Height());
				SPAssert(z >= 0); SPAssert(z < Depth());
				uint64_t mask = 1ULL << z;
				uint64_t value = solidMap[ QuantizeZ(z) ][x][y];
				bool changed = false;
				if((value & mask) != (solid ? mask : 0ULL)){
					changed = true;
					value &= ~mask;
					if(solid)
						value |= mask;
					solidMap[QuantizeZ(z)][x][y] = value;
				}
				if(solid){
					if(color != colorMap[x][y][z]){
						changed = true;
						colorMap[x][y][z] = color;
					}
				}
				if(!unsafe) {
					if(changed){
						if(listener)
							listener->GameMapChanged(x, y, z, this);
						{
							AutoLocker guard(&listenersMutex);
							for(auto*l:listeners) {
								l->GameMapChanged(x, y, z, this);
							}
						}
					}
				}
			}
			
			void SetListener(IGameMapListener *l) {
				listener = l;
			}
			IGameMapListener *GetListener() {
				return listener;
			}
			void AddListener(IGameMapListener *);
			void RemoveListener(IGameMapListener *);
			
			bool ClipBox(int x, int y, int z);
			bool ClipWorld(int x, int y, int z);
			
			bool ClipBox(float x, float y, float z);
			bool ClipWorld(float x, float y, float z);
			
			// vanila compat
			bool CastRay(Vector3 v0, Vector3 v1,
						 float length, IntVector3& vOut);
			
			// accurate and slow ray casting
			struct RayCastResult {
				bool hit;
				bool startSolid;
				Vector3 hitPos;
				IntVector3 hitBlock;
				IntVector3 normal;
			};
			RayCastResult CastRay2(Vector3 v0, Vector3 dir,
								   int maxSteps);
		private:
			// TODO :: Dynamic alloc based on map size held on server
			
			uint64_t solidMap[ int(DefaultDepth / 64) ][DefaultWidth][DefaultHeight];
			uint32_t colorMap[DefaultWidth][DefaultHeight][DefaultDepth];
			IGameMapListener *listener;
			std::list<IGameMapListener *> listeners;
			Mutex listenersMutex;
			float cullDistance;
			
			bool IsSurface(int x, int y, int z);
		};
	}
}
