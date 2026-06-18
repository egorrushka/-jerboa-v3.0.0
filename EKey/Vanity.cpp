// Vanity.cpp
/*
 * This file is part of the VanitySearch distribution (https://github.com/JeanLucPons/VanitySearch).
 * Copyright (c) 2019 Jean Luc PONS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
// EKey-Jerboa V3.0.0  fork by egorrushka
#include "Vanity.h"
#include "Jerboa.h"
#include <cstdlib>
#include <ctime>
#define COMB_SLOTS_MAX 1000

#include "Base58.h"
#include "Bech32.h"
#include "hash/sha256.h"
#include "hash/sha512.h"
#include "IntGroup.h"
#include "Wildcard.h"
#include "Timer.h"
#include "hash/ripemd160.h"
#include <string.h>
#include <math.h>
#include <algorithm> 
#include <thread>
#include <atomic>
#include <ctime> 
#include <iostream>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <cctype>      

using namespace std;

#include <fstream>
#include <cstdio>

// _ COMB progress save/load _
static std::string combProgressFile(int gpuId) {
    return std::string("comb_progress_gpu") + std::to_string(gpuId) + ".dat";
}
static bool saveCombProgress(const std::string& fname,
    const char* vis, int slots, int pass, int cyc,
    Int& base, Int* slotArr, const int* order, int jumps,
    const int* zoneOrder, int zoneCursor, const uint8_t* zoneVisited)
{
    FILE* f = fopen(fname.c_str(),"wb"); if (!f) return false;
    uint32_t mg=0x4B4B4342, ver=3;           // ver=3 adds zone state
    fwrite(&mg,4,1,f); fwrite(&ver,4,1,f);
    fwrite(&slots,sizeof(int),1,f); fwrite(&pass,sizeof(int),1,f);
    fwrite(&cyc,sizeof(int),1,f);   fwrite(&jumps,sizeof(int),1,f);
    std::string bo=base.GetBase16(); uint32_t bl=(uint32_t)bo.size();
    fwrite(&bl,4,1,f); fwrite(bo.c_str(),bl,1,f);
    fwrite(vis,1,slots,f);
    for(int s=0;s<slots;s++){
        std::string h=slotArr[s].GetBase16(); uint32_t hl=(uint32_t)h.size();
        fwrite(&hl,4,1,f); fwrite(h.c_str(),hl,1,f);
    }
    fwrite(order,sizeof(int),slots,f);
    // _ Zone state (10 zones) _
    fwrite(zoneOrder,   sizeof(int),  10, f);   // shuffled zone indices
    fwrite(&zoneCursor, sizeof(int),   1, f);   // next zone to visit
    fwrite(zoneVisited, sizeof(uint8_t),10,f);  // which zones visited this round
    fclose(f); return true;
}
static bool loadCombProgress(const std::string& fname,
    char* vis, int slots, int& pass, int& cyc,
    Int& base, Int* slotArr, int* order, int& jumps,
    int* zoneOrder, int& zoneCursor, uint8_t* zoneVisited)
{
    FILE* f = fopen(fname.c_str(),"rb"); if (!f) return false;
    uint32_t mg,ver; fread(&mg,4,1,f);
    if(mg!=0x4B4B4342){fclose(f);return false;}
    fread(&ver,4,1,f);
    bool hasZone = (ver >= 3);
    if(ver!=2 && ver!=3){fclose(f);return false;}
    int fslots; fread(&fslots,sizeof(int),1,f);
    if(fslots!=slots){fclose(f);return false;}
    fread(&pass,sizeof(int),1,f); fread(&cyc,sizeof(int),1,f);
    fread(&jumps,sizeof(int),1,f);
    uint32_t bl; fread(&bl,4,1,f);
    std::string bo(bl,0); fread(&bo[0],bl,1,f);
    {std::vector<char> buf(bo.begin(),bo.end()); buf.push_back(0);
     base.SetBase16(buf.data());}
    memset(vis,0,slots); fread(vis,1,slots,f);
    for(int s=0;s<slots;s++){
        uint32_t hl; fread(&hl,4,1,f);
        std::string h(hl,0); fread(&h[0],hl,1,f);
        std::vector<char> buf(h.begin(),h.end()); buf.push_back(0);
        slotArr[s].SetBase16(buf.data());
    }
    fread(order,sizeof(int),slots,f);
    // _ Zone state _
    if (hasZone) {
        fread(zoneOrder,   sizeof(int),   10, f);
        fread(&zoneCursor, sizeof(int),    1, f);
        fread(zoneVisited, sizeof(uint8_t),10, f);
    } else {
        // ver=2 file: init zones to default order, unvisited
        for(int z=0;z<10;z++){zoneOrder[z]=z; zoneVisited[z]=0;}
        zoneCursor=0;
    }
    fclose(f); return true;
}


VanitySearch::VanitySearch(Secp256K1* secp, std::vector<std::string>& targets, int searchMode,
    bool stop, std::string outputFile, uint32_t maxFound, BITCRACK_PARAM* bc) : inputAddresses(targets)
{
	this->secp = secp;
	this->searchMode = searchMode;
	this->stopWhenFound = stop;
	this->outputFile = outputFile;
	this->numGPUs = 0;
	this->maxFound = maxFound;	
	this->searchType = -1;
	this->bc = bc;	

	rseed(static_cast<unsigned long>(time(NULL)));
	
	addresses.clear();

	ADDRESS_TABLE_ITEM t;
	t.found = true;
	t.items = NULL;
	for (int i = 0; i < 65536; i++)
		addresses.push_back(t);
	
	nbAddress = 0;
	onlyFull = true;

	for (int i = 0; i < (int)targets.size(); i++) 
	{
		ADDRESS_ITEM it;
		if (initAddress(targets[i], &it)) {
            
			bool* found = new bool;
			*found = false;
			it.found = found;
			
            address_t p = it.sAddress;
            if (addresses[p].items == NULL) {
                addresses[p].items = new vector<ADDRESS_ITEM>();
                addresses[p].found = false;
                usedAddress.push_back(p);
            }
            (*addresses[p].items).push_back(it);
			
			onlyFull &= it.isFull;
			nbAddress++;
		}
        
        else if (this->searchType == PUBKEY) {
            nbAddress++; 
            onlyFull = false; 
        }
	}

	if (nbAddress == 0) 
	{
		fprintf(stderr, "[ERROR] VanitySearch: nothing to search !\n");
		exit(-1);
	}

	for (int i = 0; i < (int)addresses.size(); i++) 
	{
		if (addresses[i].items) 
		{
			LADDRESS lit;
			lit.sAddress = i;
			for (int j = 0; j < (int)addresses[i].items->size(); j++) 
			{
				lit.lAddresses.push_back((*addresses[i].items)[j].lAddress);
			}
			sort(lit.lAddresses.begin(), lit.lAddresses.end());
			usedAddressL.push_back(lit);
		}
	}
	
	beta.SetBase16("7ae96a2b657c07106e64479eac3434e99cf0497512f58995c1396c28719501ee");
	lambda.SetBase16("5363ad4cc05c30e0a5261c028812645a122e22ea20816678df02967c1b23bd72");
	beta2.SetBase16("851695d49a83f8ef919bb86153cbcb16630fb68aed0a766a3ec693d68e6afa40");
	lambda2.SetBase16("ac9c52b33fa3cf1f5ad9e3fd77ed9ba4a880b9fc8ec739c2e0cfc810b51283ce");
}

string VanitySearch::format_time_long(double seconds) {
    if (seconds < 1.0 || !isfinite(seconds)) return "soon";
    if (seconds < 60.0) return "seconds";
    if (seconds < 3600.0) return "minutes";
    if (seconds < 86400.0) return "hours";
    
    double years = seconds / (86400.0 * 365.25);
    stringstream ss;
    ss << scientific << setprecision(4) << years << "y";
    return ss.str();
}

bool VanitySearch::isSingularAddress(std::string pref) {

	// check is the given address contains only 1
	bool only1 = true;
	int i = 0;
	while (only1 && i < (int)pref.length()) {
		only1 = pref.data()[i] == '1';
		i++;
	}
	return only1;
}

bool VanitySearch::initAddress(std::string& address, ADDRESS_ITEM* it) {

    
    bool isHex = address.length() > 0 && std::all_of(address.cbegin(), address.cend(), [](char c){ return std::isxdigit(c); });
    bool isCompressedPubKey = isHex && address.length() == 66 && (address.substr(0, 2) == "02" || address.substr(0, 2) == "03");
    bool isUncompressedPubKey = isHex && address.length() == 130 && address.substr(0, 2) == "04";

    if (isCompressedPubKey || isUncompressedPubKey) {
        if (searchType != -1 && searchType != PUBKEY) {
            fprintf(stdout, "[ERROR] Ignoring public key \"%s\". Cannot mix public key search (-p) with address search (-a).\n", address.c_str());
            return false;
        }
        
        searchType = PUBKEY;
        
        
        std::string x_hex = address.substr(2, 64);
        std::vector<char> x_writable(x_hex.begin(), x_hex.end());
        x_writable.push_back('\0');

        Int x_coord;
        x_coord.SetBase16(x_writable.data());

        if (isCompressedPubKey) {
            this->targetPubKeyParity = (address.substr(0, 2) == "03"); // 03 is odd (1), 02 is even (0)
        } else { // isUncompressedPubKey
            std::string y_hex = address.substr(66, 64);
            std::vector<char> y_writable(y_hex.begin(), y_hex.end());
            y_writable.push_back('\0');
            
            Int y_coord;
            y_coord.SetBase16(y_writable.data());
            
            Point p;
            p.x.Set(&x_coord);
            p.y.Set(&y_coord);
            p.z.SetInt32(1); 
            if (!secp->EC(p)) {
                fprintf(stderr, "[ERROR] Invalid uncompressed public key provided: Point is not on the curve.\n");
                exit(-1);
            }
            this->targetPubKeyParity = p.y.IsOdd();
            fprintf(stdout, "[INFO] Uncompressed public key detected. Searching for its compressed form (0%c...).\n", this->targetPubKeyParity ? '3' : '2');
        }

        memcpy(this->targetPubKeyX, x_coord.bits64, sizeof(this->targetPubKeyX));

        return false;
    }

    
	std::vector<unsigned char> result;
	string dummy1 = address;
	int nbDigit = 0;
	bool wrong = false;

	if (address.length() < 2) {
		fprintf(stdout, "Ignoring address \"%s\" (too short)\n", address.c_str());
		return false;
	}

	int aType = -1;

	switch (address.data()[0]) {
	case '1':
		aType = P2PKH;
		break;
	case '3':
		aType = P2SH;
		break;
	case 'b':
	case 'B':
		std::transform(address.begin(), address.end(), address.begin(), ::tolower);
		if (strncmp(address.c_str(), "bc1q", 4) == 0)
			aType = BECH32;
		break;
	}

	if (aType == -1) {
		fprintf(stdout, "Ignoring target \"%s\" (not a valid address or compressed/uncompressed public key)\n", address.c_str());
		return false;
	}

	if (searchType == -1) searchType = aType;
	if (aType != searchType) {
		fprintf(stdout, "Ignoring address \"%s\" (P2PKH, P2SH or BECH32 allowed at once)\n", address.c_str());
		return false;
	}

	if (aType == BECH32) {

		// BECH32
		uint8_t witprog[40];
		size_t witprog_len;
		int witver;
		const char* hrp = "bc";

		int ret = segwit_addr_decode(&witver, witprog, &witprog_len, hrp, address.c_str());

		// Try to attack a full address ?
		if (ret && witprog_len == 20) {
						
			it->isFull = true;
			memcpy(it->hash160, witprog, 20);
			it->sAddress = *(address_t*)(it->hash160);
			it->lAddress = *(addressl_t*)(it->hash160);
			it->address = (char*)address.c_str();
			it->addressLength = (int)address.length();
			return true;

		}

		if (address.length() < 5) {
			fprintf(stdout, "Ignoring address \"%s\" (too short, length<5 )\n", address.c_str());
			return false;
		}

		if (address.length() >= 36) {
			fprintf(stdout, "Ignoring address \"%s\" (too long, length>36 )\n", address.c_str());
			return false;
		}

		uint8_t data[64];
		memset(data, 0, 64);
		size_t data_length;
		if (!bech32_decode_nocheck(data, &data_length, address.c_str() + 4)) {
			fprintf(stdout, "Ignoring address \"%s\" (Only \"023456789acdefghjklmnpqrstuvwxyz\" allowed)\n", address.c_str());
			return false;
		}
		
		it->sAddress = *(address_t*)data;		
		it->isFull = false;
		it->lAddress = 0;
		it->address = (char*)address.c_str();
		it->addressLength = (int)address.length();

		return true;
	}
	else {

		// P2PKH/P2SH
		wrong = !DecodeBase58(address, result);

		if (wrong) {
			fprintf(stdout, "Ignoring address \"%s\" (0, I, O and l not allowed)\n", address.c_str());
			return false;
		}

		// Try to attack a full address ?
		if (result.size() > 21) {
			
			it->isFull = true;
			memcpy(it->hash160, result.data() + 1, 20);
			it->sAddress = *(address_t*)(it->hash160);
			it->lAddress = *(addressl_t*)(it->hash160);
			it->address = (char*)address.c_str();
			it->addressLength = (int)address.length();
			return true;
		}

		// Address containing only '1'
		if (isSingularAddress(address)) {

			if (address.length() > 21) {
				fprintf(stdout, "Ignoring address \"%s\" (Too much 1)\n", address.c_str());
				return false;
			}
			
			it->isFull = false;
			it->sAddress = 0;
			it->lAddress = 0;
			it->address = (char*)address.c_str();
			it->addressLength = (int)address.length();
			return true;
		}

		// Search for highest hash160 16bit address (most probable)
		while (result.size() < 25) {
			DecodeBase58(dummy1, result);
			if (result.size() < 25) {
				dummy1.append("1");
				nbDigit++;
			}
		}

		if (searchType == P2SH) {
			if (result.data()[0] != 5) {
				fprintf(stdout, "Ignoring address \"%s\" (Unreachable, 31h1 to 3R2c only)\n", address.c_str());
				return false;
			}
		}

		if (result.size() != 25) {
			fprintf(stdout, "Ignoring address \"%s\" (Invalid size)\n", address.c_str());
			return false;
		}

		it->sAddress = *(address_t*)(result.data() + 1);

		dummy1.append("1");
		DecodeBase58(dummy1, result);

		if (result.size() == 25) {
			it->sAddress = *(address_t*)(result.data() + 1);
			nbDigit++;
		}
		
		it->isFull = false;
		it->lAddress = 0;
		it->address = (char*)address.c_str();
		it->addressLength = (int)address.length();

		return true;
	}
}

void VanitySearch::enumCaseUnsentiveAddress(std::string s, std::vector<std::string>& list) {

	char letter[64];
	int letterpos[64];
	int nbLetter = 0;
	int length = (int)s.length();

	for (int i = 1; i < length; i++) {
		char c = s.data()[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			letter[nbLetter] = tolower(c);
			letterpos[nbLetter] = i;
			nbLetter++;
		}
	}

	int total = 1 << nbLetter;

	for (int i = 0; i < total; i++) {

		char tmp[64];
		strcpy(tmp, s.c_str());

		for (int j = 0; j < nbLetter; j++) {
			int mask = 1 << j;
			if (mask & i) tmp[letterpos[j]] = toupper(letter[j]);
			else         tmp[letterpos[j]] = letter[j];
		}

		list.push_back(string(tmp));

	}

}

// ----------------------------------------------------------------------------

void VanitySearch::output(string target, string pAddr, string pAddrHex) {

#ifdef WIN64
	WaitForSingleObject(ghMutex, INFINITE);
#else
	pthread_mutex_lock(&ghMutex);
#endif

    
    FILE* foundFile = fopen("found.txt", "a");
    if (foundFile == NULL) {
        fprintf(stderr, "\n[WARNING] Could not open found.txt for writing.\n");
    }

	
	FILE* f = stdout;
	bool needToClose = false;

	if (outputFile.length() > 0) {
		f = fopen(outputFile.c_str(), "a");
		if (f == NULL) {
			fprintf(stderr, "Cannot open %s for writing\n", outputFile.c_str());
			f = stdout;
		}
		else {
			needToClose = true;
		}
	}

    
    string paddedHex = pAddrHex;
    if (paddedHex.length() < 64) {
        paddedHex.insert(0, 64 - paddedHex.length(), '0');
    }

    
    stringstream ss;
    if (this->searchType == PUBKEY) {
        ss << "\n[!] (Pub): " << target << "\n";
        ss << "[!] (WIF): Compressed:" << pAddr << "\n";
    } else {
        ss << "\n[!] (Add): " << target << "\n";
        string wif_type = "p2pkh"; 
        if(target.length() > 0){
            if (target[0] == '3') wif_type = "p2wpkh-p2sh";
            else if (target.rfind("bc1", 0) == 0) wif_type = "p2wpkh";
        }
        ss << "[!] (WIF): " << wif_type << ":" << pAddr << "\n";
    }
    ss << "[!] (HEX): 0x" << paddedHex << "\n";

    string output_str = ss.str();

    
    
    
	fprintf(stdout, "\n%s", output_str.c_str());
    fflush(stdout);

    
	if (f != stdout) {
        fprintf(f, "%s", output_str.c_str());
        fflush(f);
    }
    
    
    if (foundFile != NULL) {
        fprintf(foundFile, "%s", output_str.c_str());
        fflush(foundFile);
    }

    
	if (needToClose) {
        fclose(f);
    }
    if (foundFile != NULL) {
        fclose(foundFile);
    }

    // Save snapshot file on key found
    {
        char snapName[64];
        time_t snapTime = time(NULL);
        struct tm* st = localtime(&snapTime);
        snprintf(snapName, sizeof(snapName), "FOUND_%04d%02d%02d_%02d%02d%02d.txt",
                 st->tm_year+1900, st->tm_mon+1, st->tm_mday,
                 st->tm_hour, st->tm_min, st->tm_sec);
        FILE* snap = fopen(snapName, "w");
        if (snap) {
            fprintf(snap, "=== KEY FOUND ===\n");
            fprintf(snap, "Target  : %s\n", target.c_str());
            fprintf(snap, "WIF     : %s\n", pAddr.c_str());
            fprintf(snap, "HEX     : 0x%s\n", paddedHex.c_str());
            if (bc) {
                fprintf(snap, "Mode    : %s\n",
                        bc->combSequential ? "COMB SWEEP" :
                        bc->combMode       ? "COMB RANDOM" : "Sequential");
                fprintf(snap, "Chunk   : 0x%s -> 0x%s\n",
                        bc->ksStart.GetBase16().c_str(),
                        bc->ksFinish.GetBase16().c_str());
                if (bc->combMode && !bc->combSequential)
                    fprintf(snap, "Slot    : %d/%d\n",
                            bc->combCurrentPass, bc->combSlotsCount);
                if (bc->combInterleaveStep > 0.0)
                    fprintf(snap, "Cycle   : %d  (step %.0f%%)\n",
                            bc->combCycleNum, bc->combInterleaveStep);
            }
            fclose(snap);
            printf("[+] Snapshot: %s\n", snapName);
        }
    }

#ifdef WIN64
	ReleaseMutex(ghMutex);
#else
	pthread_mutex_unlock(&ghMutex);
#endif
}

void VanitySearch::updateFound() {

	// Check if all addresses has been found
	// Needed only if stopWhenFound is asked
	if (stopWhenFound) 	{

		bool allFound = true;
		for (int i = 0; i < (int)usedAddress.size(); i++) {
			bool iFound = true;
			address_t p = usedAddress[i];
			if (!addresses[p].found) {
				if (addresses[p].items) {
					for (int j = 0; j < (int)addresses[p].items->size(); j++) {
						iFound &= *((*addresses[p].items)[j].found);
					}
				}
				addresses[usedAddress[i]].found = iFound;
			}
			allFound &= iFound;
		}

		endOfSearch = allFound;		
	}		
}

bool VanitySearch::checkPrivKey(string addr, Int& key, int32_t incr, int endomorphism, bool mode) {

	Int k(&key);	

	if (incr < 0) {
		k.Add((uint64_t)(-incr));
		k.Neg();
		k.Add(&secp->order);		
	}
	else {
		k.Add((uint64_t)incr);
	}

	// Endomorphisms
	switch (endomorphism) {
	case 1:
		k.ModMulK1order(&lambda);		
		break;
	case 2:
		k.ModMulK1order(&lambda2);		
		break;
	}

	// Check addresses
	Point p = secp->ComputePublicKey(&k);	

	string chkAddr = secp->GetAddress(searchType, mode, p);
	if (chkAddr != addr) {

		// Key may be the opposite one (negative zero or compressed key)
		k.Neg();
		k.Add(&secp->order);
		p = secp->ComputePublicKey(&k);
		
		string chkAddr = secp->GetAddress(searchType, mode, p);
		if (chkAddr != addr) {
			fprintf(stdout, "\nWarning, wrong private key generated !\n");
			fprintf(stdout, "  Addr :%s\n", addr.c_str());
			fprintf(stdout, "  Check:%s\n", chkAddr.c_str());
			fprintf(stdout, "  Endo:%d incr:%d comp:%d\n", endomorphism, incr, mode);
			return false;
		}

	}

	output(chkAddr, secp->GetPrivAddress(mode, k), k.GetBase16());

	return true;
}

void VanitySearch::checkAddrSSE(uint8_t* h1, uint8_t* h2, uint8_t* h3, uint8_t* h4,
	int32_t incr1, int32_t incr2, int32_t incr3, int32_t incr4,
	Int& key, int endomorphism, bool mode) {

	vector<string> addr = secp->GetAddress(searchType, mode, h1, h2, h3, h4);

	for (int i = 0; i < (int)inputAddresses.size(); i++) {

		if (Wildcard::match(addr[0].c_str(), inputAddresses[i].c_str())) {

			// Found it !      
			if (checkPrivKey(addr[0], key, incr1, endomorphism, mode)) {
				nbFoundKey++;
				//patternFound[i] = true;
				updateFound();
			}
		}

		if (Wildcard::match(addr[1].c_str(), inputAddresses[i].c_str())) {

			// Found it !      
			if (checkPrivKey(addr[1], key, incr2, endomorphism, mode)) {
				nbFoundKey++;
				//patternFound[i] = true;
				updateFound();
			}
		}

		if (Wildcard::match(addr[2].c_str(), inputAddresses[i].c_str())) {

			// Found it !      
			if (checkPrivKey(addr[2], key, incr3, endomorphism, mode)) {
				nbFoundKey++;
				//patternFound[i] = true;
				updateFound();
			}
		}

		if (Wildcard::match(addr[3].c_str(), inputAddresses[i].c_str())) {

			// Found it !      
			if (checkPrivKey(addr[3], key, incr4, endomorphism, mode)) {
				nbFoundKey++;
				//patternFound[i] = true;
				updateFound();
			}
		}
	}
}

void VanitySearch::checkAddr(int prefIdx, uint8_t* hash160, Int& key, int32_t incr, int endomorphism, bool mode) {
	
	vector<ADDRESS_ITEM>* pi = addresses[prefIdx].items;	

	if (onlyFull) {

		// Full addresses
		for (int i = 0; i < (int)pi->size(); i++) {

			if (stopWhenFound && *((*pi)[i].found))
				continue;

			if (ripemd160_comp_hash((*pi)[i].hash160, hash160)) {

				// Found it !
				*((*pi)[i].found) = true;
				// You believe it ?
				if (checkPrivKey(secp->GetAddress(searchType, mode, hash160), key, incr, endomorphism, mode)) {
					nbFoundKey++;
					updateFound();
				}

			}

		}

	}
	else {
		char a[64];

		string addr = secp->GetAddress(searchType, mode, hash160);

		for (int i = 0; i < (int)pi->size(); i++) {

			if (stopWhenFound && *((*pi)[i].found))
				continue;

			strncpy(a, addr.c_str(), (*pi)[i].addressLength);
			a[(*pi)[i].addressLength] = 0;

			if (strcmp((*pi)[i].address, a) == 0) {

				// Found it !
				*((*pi)[i].found) = true;
				if (checkPrivKey(addr, key, incr, endomorphism, mode)) {
					nbFoundKey++;
					updateFound();
				}

			}

		}

	}

}

#ifdef WIN64
DWORD WINAPI _FindKeyGPU(LPVOID lpParam) {
#else
void* _FindKeyGPU(void* lpParam) {
#endif
	TH_PARAM* p = (TH_PARAM*)lpParam;
	p->obj->FindKeyGPU(p);
	return 0;
}

void VanitySearch::checkAddresses(bool compressed, Int key, int i, Point p1) {

	unsigned char h0[20];
	Point pte1[1];
	Point pte2[1];

	// Point
	secp->GetHash160(searchType, compressed, p1, h0);
	address_t pr0 = *(address_t*)h0;
	if (addresses[pr0].items)
		checkAddr(pr0, h0, key, i, 0, compressed);	
}

void VanitySearch::checkAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4) {

	unsigned char h0[20];
	unsigned char h1[20];
	unsigned char h2[20];
	unsigned char h3[20];
	Point pte1[4];
	Point pte2[4];
	address_t pr0;
	address_t pr1;
	address_t pr2;
	address_t pr3;

	// Point -------------------------------------------------------------------------
	secp->GetHash160(searchType, compressed, p1, p2, p3, p4, h0, h1, h2, h3);	

	pr0 = *(address_t*)h0;
	pr1 = *(address_t*)h1;
	pr2 = *(address_t*)h2;
	pr3 = *(address_t*)h3;

	if (addresses[pr0].items)
		checkAddr(pr0, h0, key, i, 0, compressed);
	if (addresses[pr1].items)
		checkAddr(pr1, h1, key, i + 1, 0, compressed);
	if (addresses[pr2].items)
		checkAddr(pr2, h2, key, i + 2, 0, compressed);
	if (addresses[pr3].items)
		checkAddr(pr3, h3, key, i + 3, 0, compressed);	
}

void VanitySearch::getGPUStartingKeys(Int& tRangeStart, Int& tRangeEnd, int groupSize, int nbThread, Point *p, uint64_t Progress) {
		
	uint32_t grp_startkeys = nbThread/256;

	//New setting key by fixedpaul using addition on secp with batch modular inverse, super fast, multithreading not needed

	Int stepThread;
	Int numthread;

	stepThread.Set(&bc->ksFinish);
	stepThread.Sub(&bc->ksStart);
	stepThread.AddOne();
	numthread.SetInt32(nbThread);
	stepThread.Div(&numthread);

	Point Pdouble;
	Int kDouble;

	kDouble.Set(&stepThread);
	kDouble.Mult(grp_startkeys);
	Pdouble = secp->ComputePublicKey(&kDouble);

	Point P_start;
	Int kStart;

	kStart.Set(&stepThread);
	kStart.Mult(grp_startkeys / 2);
	kStart.Add(groupSize / 2 + Progress);

	

	P_start = secp->ComputePublicKey(&kStart);

	p[grp_startkeys / 2] = secp->ComputePublicKey(&tRangeStart);
	p[grp_startkeys / 2] = secp->AddDirect(p[grp_startkeys / 2], P_start);

	Int key_delta;
	Point* p_delta;
	p_delta = new Point[grp_startkeys / 2];

	key_delta.Set(&stepThread);

	

	p_delta[0] = secp->ComputePublicKey(&key_delta);
	key_delta.Add(&stepThread);
	p_delta[1] = secp->ComputePublicKey(&key_delta);

	for (size_t i = 2; i < grp_startkeys / 2; i++) {
		p_delta[i] = secp->AddDirect(p_delta[i - 1], p_delta[0]);
	}

	Int* dx;
	Int* subp;

	subp = new Int[grp_startkeys / 2 + 1];
	dx = new Int[grp_startkeys / 2 + 1];

	uint32_t j;
	//uint32_t i;

	for (size_t i = grp_startkeys / 2; i < nbThread; i += grp_startkeys) {

		double percentage = (100.0 * (double)(i + grp_startkeys / 2)) / (double)(nbThread);
		printf("Setting starting keys... [%.2f%%] \r", percentage);
		fflush(stdout);

		for (j = 0; j < grp_startkeys / 2; j++) {
			dx[j].ModSub(&p_delta[j].x, &p[i].x);
		}
		dx[grp_startkeys / 2].ModSub(&Pdouble.x, &p[i].x);

		Int newValue;
		Int inverse;

		subp[0].Set(&dx[0]);
		for (size_t j = 1; j < grp_startkeys / 2 + 1; j++) {
			subp[j].ModMulK1(&subp[j - 1], &dx[j]);
		}

		// Do the inversion
		inverse.Set(&subp[grp_startkeys / 2]);
		inverse.ModInv();

		for (j = grp_startkeys / 2; j > 0; j--) {
			newValue.ModMulK1(&subp[j - 1], &inverse);
			inverse.ModMulK1(&dx[j]);
			dx[j].Set(&newValue);
		}

		dx[0].Set(&inverse);

		Int _s;
		Int _p;
		Int dy;
		Int syn;
		syn.Set(&p[i].y);
		syn.ModNeg();

		for (j = 0; j < grp_startkeys / 2 - 1; j++) {

			dy.ModSub(&p_delta[j].y, &p[i].y);
			_s.ModMulK1(&dy, &dx[j]);

			_p.ModSquareK1(&_s);

			p[i + j + 1].x.ModSub(&_p, &p[i].x);
			p[i + j + 1].x.ModSub(&p_delta[j].x);

			p[i + j + 1].y.ModSub(&p_delta[j].x, &p[i + j + 1].x);
			p[i + j + 1].y.ModMulK1(&_s);
			p[i + j + 1].y.ModSub(&p_delta[j].y);

			dy.ModSub(&syn, &p_delta[j].y);
			_s.ModMulK1(&dy, &dx[j]);

			_p.ModSquareK1(&_s);

			p[i - j - 1].x.ModSub(&_p, &p[i].x);
			p[i - j - 1].x.ModSub(&p_delta[j].x);

			p[i - j - 1].y.ModSub(&p[i - j - 1].x, &p_delta[j].x);
			p[i - j - 1].y.ModMulK1(&_s);
			p[i - j - 1].y.ModSub(&p_delta[j].y, &p[i - j - 1].y);
		}

		dy.ModSub(&syn, &p_delta[j].y);
		_s.ModMulK1(&dy, &dx[j]);

		_p.ModSquareK1(&_s);

		p[i - j - 1].x.ModSub(&_p, &p[i].x);
		p[i - j - 1].x.ModSub(&p_delta[j].x);

		p[i - j - 1].y.ModSub(&p[i - j - 1].x, &p_delta[j].x);
		p[i - j - 1].y.ModMulK1(&_s);
		p[i - j - 1].y.ModSub(&p_delta[j].y, &p[i - j - 1].y);

		if (i + grp_startkeys < nbThread) {

			dy.ModSub(&Pdouble.y, &p[i].y);
			_s.ModMulK1(&dy, &dx[grp_startkeys / 2]);

			_p.ModSquareK1(&_s);

			p[i + grp_startkeys].x.ModSub(&_p, &p[i].x);
			p[i + grp_startkeys].x.ModSub(&Pdouble.x);

			p[i + grp_startkeys].y.ModSub(&Pdouble.x, &p[i + grp_startkeys].x);
			p[i + grp_startkeys].y.ModMulK1(&_s);
			p[i + grp_startkeys].y.ModSub(&Pdouble.y);
		}
	}

	delete[] subp;
	delete[] dx;
	delete[] p_delta;
}

void VanitySearch::FindKeyGPU(TH_PARAM* ph) {

    // EKey-Jerboa V3.0.0 by egorrushka
    // Deep is the only engine — chunk split by Nth hex symbol (-D3..-D5).
    findKeyGPU_Deep(ph); return;

    bool ok = true;
    double t0 = 0.0, ttot = 0.0;
    uint64_t keys_n = 0;
    vector<ITEM> found;
    
    Int total_keyspace;
    total_keyspace.Sub(&bc->ksFinish, &bc->ksStart);
    total_keyspace.AddOne();

    ph->hasStarted = true;
    endOfSearch = false; 

    
    while (!endOfSearch) {

        
        if (Pause) {
            if (!Paused) { 
                printf("\n[+] Paused. Press 'p' to resume or Ctrl+C to exit.\r");
                fflush(stdout);
                Paused = true;
            }
            Timer::SleepMillis(100); 
            continue; 
        }

        

        // ====================================================================
        
        // ====================================================================
        if (Paused) { 
            printf("\n[+] Resuming search...\n");
            fflush(stdout);
        }

        GPUEngine g(ph->gpuId, maxFound);
        
        if (!g.IsInitialised()) {
            fprintf(stderr, "\n[ERROR] Failed to initialize GPU Engine. Exiting thread.\n");
            break; 
        }
        
        if (!bc->combMode || (bc->combCurrentPass == 0 && bc->combCycleNum == 0))
            printf("[+] GPU: %s\n", g.deviceName.c_str());
        fflush(stdout);
        
        if (this->searchType == PUBKEY) {
            g.SetTargetPublicKey(this->targetPubKeyX, this->targetPubKeyParity);
        } else {
            g.SetSearchType(searchType);
            g.SetAddress(usedAddressL, nbAddress);

            // ----------------------------------------------------------------
            // OPT: Single-target fast path
            // Если задан ровно 1 полный адрес (puzzle hunting) — загружаем
            // его hash160 в constant memory GPU. CheckPoint_Opt будет
            // сравнивать в регистрах без единого обращения к global memory
            // для 99.9999999% ключей. Активирует SINGLE_TARGET_MODE.
            // ----------------------------------------------------------------
            if (nbAddress == 1 && onlyFull &&
                !usedAddress.empty()) {
                int p = usedAddress[0];
                if (addresses[p].items && !addresses[p].items->empty()) {
                    uint8_t* h160 = (*addresses[p].items)[0].hash160;
                    g.SetHash160Target(h160, true);
                }
            }
        }

        int numThreadsGPU = g.GetNbThread();
        int STEP_SIZE = g.GetStepSize();
        Point* publicKeys = new Point[numThreadsGPU];

        // _ COMB: save chunk bounds once _
        static Int combChunkStart, combChunkEnd;
        static bool combChunkSaved = false;
        if (!combChunkSaved && bc->combMode) {
            combChunkStart.Set(&bc->ksStart);
            combChunkEnd.Set(&bc->ksFinish);
            combChunkSaved = true;
        }

        // _ COMB: one-time initialization _
        // Divides chunk into bc->combSlotsCount slots, Fisher-Yates shuffles them.
        // Uses time-based slot switching (combJumpMinutes per slot).
        if (bc->combMode && !bc->combDone && bc->combCoverage.IsZero()) {

            // _ Reserve gap = one slot width at chunk end for interleave _
            // Effective chunk end = combChunkEnd - gap, so comb always fits
            Int effectiveChunkEnd; effectiveChunkEnd.Set(&combChunkEnd);
            if (bc->combInterleaveStep > 0.0 && !bc->combSequential) {
                Int chkSzTmp; chkSzTmp.Set(&combChunkEnd);
                chkSzTmp.Sub(&combChunkStart); chkSzTmp.AddOne();
                Int nsTmp; nsTmp.SetInt32(bc->combSlotsCount);
                Int gapTmp; gapTmp.Set(&chkSzTmp); gapTmp.Div(&nsTmp);
                effectiveChunkEnd.Sub(&gapTmp);
                printf("[+] Interleave: reserved %.4f%% gap at chunk end\n",
                       100.0 / bc->combSlotsCount);
            }

            // SWEEP without interleave: treat as pure sequential over full chunk
            if (bc->combSequential && bc->combInterleaveStep == 0.0) {
                bc->ksStart.Set(&combChunkStart);
                bc->ksFinish.Set(&combChunkEnd);
                bc->combCoverage.SetInt32(1); // mark as initialized (non-zero)
                printf("[+] COMB SWEEP: Sequential scan  (0x%s -> 0x%s)\n",
                       bc->ksStart.GetBase16().c_str(),
                       bc->ksFinish.GetBase16().c_str());
                fflush(stdout);
                goto sweep_skip_slot_init;
            }

            Int chunkSz;
            chunkSz.Set(&combChunkEnd);
            chunkSz.Sub(&combChunkStart);
            chunkSz.AddOne();

            // coverage = chunk_size / bc->combSlotsCount  (keys per slot)
            Int nslots; nslots.SetInt32(bc->combSlotsCount);
            bc->combCoverage.Set(&chunkSz);
            bc->combCoverage.Div(&nslots);
            if (bc->combCoverage.IsZero()) bc->combCoverage.SetInt32(1);

            // Init order array BEFORE shuffle (slot s _ original index s)
            for (int s = 0; s < bc->combSlotsCount; s++) bc->combSlotOrder[s] = s;

            // Coverage based on effective chunk (full chunk when no interleave)
            {
                Int effSz; effSz.Set(&effectiveChunkEnd);
                effSz.Sub(&combChunkStart); effSz.AddOne();
                Int ns; ns.SetInt32(bc->combSlotsCount);
                bc->combCoverage.Set(&effSz);
                bc->combCoverage.Div(&ns);
                if (bc->combCoverage.IsZero()) bc->combCoverage.SetInt32(1);
            }

            // Build slots: offsets 0, cov, 2*cov, ..., (N-1)*cov
            for (int s = 0; s < bc->combSlotsCount; s++) {
                bc->combSlots[s].Set(&bc->combCoverage);
                bc->combSlots[s].Mult((uint64_t)(unsigned)s);
            }

            if (bc->combSequential) {
                // SWEEP: slots already in order 0,1,2,...,99 _ no shuffle
                printf("[+] COMB SWEEP: %s  (%d slots, slot_size=0x%s)\n",
                       (bc->combInterleaveStep==0.0 ? "Sequential (no interleave)" : "Interleaved"),
                       bc->combSlotsCount, bc->combCoverage.GetBase16().c_str());
                printf("[+] Slot 0 = 0%%,  Slot 1 = 1%%,  ...  Slot 99 = 99%%\n");
                printf("[+] Key at N%% of chunk will be found at slot ~N\n");
            } else {
                // RANDOM: Fisher-Yates true random shuffle
                srand((unsigned)time(NULL));
                for (int s = bc->combSlotsCount - 1; s > 0; s--) {
                    int j = rand() % (s + 1);
                    // Swap slot offsets
                    Int tmp; tmp.Set(&bc->combSlots[s]);
                    bc->combSlots[s].Set(&bc->combSlots[j]);
                    bc->combSlots[j].Set(&tmp);
                    // Swap original indices (so bar shows real chunk position)
                    int ti = bc->combSlotOrder[s];
                    bc->combSlotOrder[s] = bc->combSlotOrder[j];
                    bc->combSlotOrder[j] = ti;
                }
                printf("[+] COMB RANDOM: %d slots x 1%%  slot_size=0x%s\n",
                       bc->combSlotsCount, bc->combCoverage.GetBase16().c_str());
                printf("[+] Slot[%d]..Slot[%d]: %d slots shuffled\n",
                           0, bc->combSlotsCount-1, bc->combSlotsCount);
            }

            bc->combCurrentPass = 0;

            // SWEEP without -I: use full chunk as single range (pure sequential)
            if (bc->combSequential && bc->combInterleaveStep == 0.0) {
                bc->ksStart.Set(&combChunkStart);
                bc->ksFinish.Set(&combChunkEnd);
                bc->combCurrentPass = 0;
                printf("[+] COMB SWEEP: Sequential (no interleave) _ full chunk scan\n");
                fflush(stdout);
            } else {
                // Set ksStart/ksFinish to first slot
                bc->ksStart.Set(&combChunkStart);
                bc->ksStart.Add(&bc->combSlots[0]);
                bc->ksFinish.Set(&bc->ksStart);
                bc->ksFinish.Add(&bc->combCoverage);
                bc->ksFinish.SubOne();
                if (bc->ksFinish.IsGreater(&combChunkEnd)) bc->ksFinish.Set(&combChunkEnd);
            }
            fflush(stdout);
        }
        sweep_skip_slot_init:;

        Int stepThread;
        // stepThread must match getGPUStartingKeys internal formula:
        // both use (ksFinish - ksStart + 1) / numThreads
        {
            Int len; len.Set(&bc->ksFinish);
            len.Sub(&bc->ksStart);
            len.AddOne();
            Int nt; nt.SetInt32(numThreadsGPU);
            stepThread.Set(&len);
            stepThread.Div(&nt);
        }

        // _ Load COMB progress BEFORE GPU starts _
        static bool    combResumeLoaded  = false;
        static char    tmpVis[2048]      = {0};
        static int     tmpJumps          = 0;
        // _ Persistent zone state (lives across GPU re-inits) _
        static int     gZoneOrder[10]    = {0,1,2,3,4,5,6,7,8,9};
        static int     gZoneCursor       = 0;
        static uint8_t gZoneVisited[10]  = {0};  // 1 = visited this round
        static bool    gZoneInitDone     = false;

        if (!combResumeLoaded && bc->combMode && !bc->combSequential
                && bc->combCurrentPass==0 && bc->combCycleNum==0) {
            combResumeLoaded=true;
            int rPass=0,rCyc=0,rJumps=0;
            Int rBase; rBase.SetInt32(0);
            std::string pf=combProgressFile(ph->gpuId);
            if(loadCombProgress(pf,tmpVis,bc->combSlotsCount,rPass,rCyc,
                                rBase,bc->combSlots,bc->combSlotOrder,rJumps,
                                gZoneOrder,gZoneCursor,gZoneVisited)){
                bc->combCurrentPass=rPass; bc->combCycleNum=rCyc;
                bc->combBaseOffset.Set(&rBase); tmpJumps=rJumps;
                bc->ksStart.Set(&combChunkStart);
                Int soff; soff.Set(&bc->combSlots[rPass]);
                bc->ksStart.Add(&soff);
                bc->ksFinish.Set(&bc->ksStart);
                bc->ksFinish.Add(&bc->combCoverage); bc->ksFinish.SubOne();
                if(bc->ksFinish.IsGreater(&combChunkEnd)) bc->ksFinish.Set(&combChunkEnd);
                gZoneInitDone = true;
                printf("[+] Progress restored: slot %d/%d CYC:%d done:%d zone:%d/10\n",
                       rPass,bc->combSlotsCount,rCyc,rJumps,gZoneCursor);
            } else {
                printf("[+] No progress file, starting fresh.\n");
            }
            fflush(stdout);
        }

        double setup_t0 = Timer::get_tick();
        getGPUStartingKeys(bc->ksStart, bc->ksFinish, g.GetGroupSize(), numThreadsGPU, publicKeys, (uint64_t)(1ULL * idxcount * STEP_SIZE));
        ok = g.SetKeys(publicKeys);
        delete[] publicKeys;
        double setup_time = Timer::get_tick() - setup_t0;
        if (!bc->combMode || (bc->combCurrentPass == 0 && bc->combCycleNum == 0))
            printf("[+] Starting keys set in %.2f seconds\n", setup_time);
        fflush(stdout);

        Paused = false;
        t0 = Timer::get_tick();

        // _ COMB: record slot start time _
        double combSlotT0 = Timer::get_tick();
        // jumpSeconds: 0 means auto (batch-count based), else time-based
        double jumpSeconds = (bc->combMode && bc->combJumpMinutes > 0.0)
                             ? (bc->combJumpMinutes * 60.0) : 0.0;
        // auto batches: cover full slot
        int combAutoBatches = 4;  // default safety value
        if (bc->combMode && jumpSeconds == 0.0) {
            // Each thread covers exactly stepThread keys.
            // Batches needed = ceil(stepThread / STEP_SIZE) + 2 (safety margin).
            // Using stepThread (not total coverage) avoids under-counting.
            uint64_t st = stepThread.IsZero()
                          ? (uint64_t)STEP_SIZE
                          : stepThread.bits64[0];
            combAutoBatches = (int)((st + (uint64_t)STEP_SIZE - 1)
                                    / (uint64_t)STEP_SIZE) + 2;
            if (combAutoBatches < 3) combAutoBatches = 3;
        }
        int combBatchCount = 0;

        while (ok && !endOfSearch && !Pause) {
            ok = g.Launch(found, true);
            idxcount += 1;
            combBatchCount++;

            // _ COMB RANDOM: check if current slot is done _
            // SWEEP always bypasses this block (pure sequential)
            if (bc->combMode && !bc->combSequential && !bc->combDone) {
                bool slotDone = false;
                if (jumpSeconds > 0.0) {
                    slotDone = (Timer::get_tick() - combSlotT0) >= jumpSeconds;
                } else {
                    slotDone = (combBatchCount >= combAutoBatches);
                }

                if (slotDone) {
                    bc->combCurrentPass++;
                    combBatchCount = 0;
                    idxcount = 0;

                    if (bc->combCurrentPass >= bc->combSlotsCount) {
                        printf("\r\033[2K[+] COMB cycle complete (%d slots).\n",
                               bc->combSlotsCount);
                        fflush(stdout);

                        if (bc->combInterleaveStep <= 0.0) {
                            // _ _: _ _ _ _ _ _ _ _ _
                            printf("\r\033[2K[+] _ _ _ (2000 _ _ 1 _). _.\n");
                            fflush(stdout);
                            endOfSearch = true;
                            break;
                        }

                        // _ _: reshuffling with random zone jump _
                        printf("\r\033[2K[+] COMB reshuffling for next zone jump...\n");
                        fflush(stdout);

                        bc->combCycleNum++;

                        // _ RANDOM COMB JUMP: 10-_ _ _ _
                        // _ _ _ 10 _ _ (_ _ _).
                        // _ _ _ _ _ _ _.
                        // _ _ _ 10 _ _ _ _ _.
                        if (bc->combInterleaveStep > 0.0) {

                            // _ _ _ _ = chunkSize / 10, _ _ _ _
                            // _ _ _ combCoverage (_ _)         _
                            Int chunkSize2;
                            chunkSize2.Set(&combChunkEnd);
                            chunkSize2.Sub(&combChunkStart);
                            chunkSize2.AddOne();

                            Int totalCombWidth2;
                            totalCombWidth2.Set(&bc->combCoverage);
                            totalCombWidth2.Mult((uint64_t)(unsigned)bc->combSlotsCount);

                            // zoneSize = floor(chunkSize / 10), _ _ combCoverage
                            Int zoneSize; zoneSize.Set(&chunkSize2);
                            Int ten; ten.SetInt32(10);
                            zoneSize.Div(&ten);
                            // _ _ _ _ combCoverage
                            if (!bc->combCoverage.IsZero()) {
                                Int rem; rem.Set(&zoneSize); rem.Mod(&bc->combCoverage);
                                if (!rem.IsZero()) zoneSize.Sub(&rem);
                            }
                            if (zoneSize.IsZero()) zoneSize.Set(&totalCombWidth2);

                            // _ zoneOrder _ zoneCursor _ _ static _ _ _
                            // _ _ _ _ offset _ _ _.
                            // _ _ _ static _ _ _ static _
                            // (_ _ _ _ _, _ _ _ _
                            //  _ _ _). _ _ _ _:
                            if (!gZoneInitDone || gZoneCursor >= 10) {
                                for (int z = 9; z > 0; z--) {
                                    int jz = rand() % (z + 1);
                                    int tv = gZoneOrder[z];
                                    gZoneOrder[z]  = gZoneOrder[jz];
                                    gZoneOrder[jz] = tv;
                                }
                                memset(gZoneVisited, 0, sizeof(gZoneVisited));
                                gZoneCursor  = 0;
                                gZoneInitDone = true;
                            }

                            // _ _ _ _ _ _
                            int zoneIdx = gZoneOrder[gZoneCursor];
                            // _ _ _
                            if (zoneIdx >= 0 && zoneIdx < 10)
                                gZoneVisited[zoneIdx] = 1;
                            gZoneCursor++;

                            // offset = zoneIdx * zoneSize
                            Int zoneOffset; zoneOffset.Set(&zoneSize);
                            zoneOffset.Mult((uint64_t)(unsigned)zoneIdx);

                            // _ _ _ _ _ _:
                            // maxOffset = chunkSize - totalCombWidth
                            Int maxOffset2; maxOffset2.Set(&chunkSize2);
                            if (maxOffset2.IsGreater(&totalCombWidth2))
                                maxOffset2.Sub(&totalCombWidth2);
                            else
                                maxOffset2.SetInt32(0);

                            if (zoneOffset.IsGreater(&maxOffset2))
                                zoneOffset.Set(&maxOffset2);

                            bc->combBaseOffset.Set(&zoneOffset);

                            // Rebuild slots: baseOffset + i*coverage
                            for (int s = 0; s < bc->combSlotsCount; s++) {
                                bc->combSlotOrder[s] = s;
                                Int off; off.Set(&bc->combCoverage);
                                off.Mult((uint64_t)(unsigned)s);
                                off.Add(&bc->combBaseOffset);
                                bc->combSlots[s].Set(&off);
                            }

                            // Fisher-Yates shuffle
                            for (int s = bc->combSlotsCount - 1; s > 0; s--) {
                                int j = rand() % (s + 1);
                                Int tmp; tmp.Set(&bc->combSlots[s]);
                                bc->combSlots[s].Set(&bc->combSlots[j]);
                                bc->combSlots[j].Set(&tmp);
                                int ti = bc->combSlotOrder[s];
                                bc->combSlotOrder[s] = bc->combSlotOrder[j];
                                bc->combSlotOrder[j] = ti;
                            }

                            // _ _ _ _ _ _
                            Int absStart2; absStart2.Set(&combChunkStart);
                            absStart2.Add(&bc->combBaseOffset);
                            printf("\r\033[2K[+] COMB jump #%d  zone %d/10 _ 0x%s\n",
                                   bc->combCycleNum, zoneIdx,
                                   absStart2.GetBase16().c_str());
                            fflush(stdout);

                        } else {
                            // No interleave: reshuffle same positions
                            for (int s = bc->combSlotsCount - 1; s > 0; s--) {
                                int j = rand() % (s + 1);
                                Int tmp; tmp.Set(&bc->combSlots[s]);
                                bc->combSlots[s].Set(&bc->combSlots[j]);
                                bc->combSlots[j].Set(&tmp);
                                int ti = bc->combSlotOrder[s];
                                bc->combSlotOrder[s] = bc->combSlotOrder[j];
                                bc->combSlotOrder[j] = ti;
                            }
                        }

                        bc->combCurrentPass = 0;
                        idxcount = 0;

                        // Set ksStart/ksFinish to first slot of new cycle
                        bc->ksStart.Set(&combChunkStart);
                        bc->ksStart.Add(&bc->combSlots[0]);
                        bc->ksFinish.Set(&bc->ksStart);
                        bc->ksFinish.Add(&bc->combCoverage);
                        bc->ksFinish.SubOne();
                        if (bc->ksFinish.IsGreater(&combChunkEnd))
                            bc->ksFinish.Set(&combChunkEnd);

                        // _ Save progress at cycle boundary _
                        if (bc->combMode && !bc->combSequential) {
                            static char visCyc[2048] = {0};
                            static int  jumpsCyc = 0;
                            jumpsCyc++;
                            saveCombProgress(combProgressFile(ph->gpuId),
                                visCyc, bc->combSlotsCount,
                                bc->combCurrentPass, bc->combCycleNum,
                                bc->combBaseOffset, bc->combSlots,
                                bc->combSlotOrder, jumpsCyc,
                                gZoneOrder, gZoneCursor, gZoneVisited);
                        }

                        combSlotT0 = Timer::get_tick();
                        t_Paused = 0.0;
                        t0 = Timer::get_tick();
                        break;  // re-init GPU with new ksStart/ksFinish
                    } else {
                        // Move to next slot
                        Int nextOff; nextOff.Set(&bc->combSlots[bc->combCurrentPass]);
                        bc->ksStart.Set(&combChunkStart);
                        bc->ksStart.Add(&nextOff);
                        bc->ksFinish.Set(&bc->ksStart);
                        bc->ksFinish.Add(&bc->combCoverage);
                        bc->ksFinish.SubOne();
                        // clamp to chunk end
                        if (bc->ksFinish.IsGreater(&combChunkEnd))
                            bc->ksFinish.Set(&combChunkEnd);

                        int pct = bc->combCurrentPass * 100 / bc->combSlotsCount;
                        const char* jtype = bc->combSequential ? "SWEEP" : "COMB";

                        // _ Static progress bar (updates on jump, overwrites with \033[NA) _
                        {
                            const int COLS=50, ROWS=40; // 50*40=2000
                            static char visited[2048] = {0};
                            static int  totalJumps    = 0;
                            static int  lastCycleNum  = -1;

                            // _ Restore from file on first entry _
                            // Must happen BEFORE the cycle-change check so we can set
                            // lastCycleNum = combCycleNum and avoid the memset wiping data.
                            if (totalJumps == 0 && tmpJumps > 0) {
                                totalJumps   = tmpJumps;
                                memcpy(visited, tmpVis, sizeof(visited));
                                // Pre-set lastCycleNum so the cycle-change block below
                                // does NOT trigger memset(visited,0) on this same entry.
                                lastCycleNum = bc->combCycleNum;
                            }

                            totalJumps++;

                            // When cycle actually changes _ clear visited map, resync zones
                            if (bc->combCycleNum != lastCycleNum) {
                                memset(visited, 0, sizeof(visited));
                                lastCycleNum = bc->combCycleNum;
                                if (!gZoneInitDone || gZoneCursor >= 10) {
                                    for (int z = 9; z > 0; z--) {
                                        int jz = rand() % (z + 1);
                                        int tv = gZoneOrder[z];
                                        gZoneOrder[z] = gZoneOrder[jz];
                                        gZoneOrder[jz] = tv;
                                    }
                                    memset(gZoneVisited, 0, sizeof(gZoneVisited));
                                    gZoneCursor   = 0;
                                    gZoneInitDone = true;
                                }
                            }

                            // _ Save after each jump _
                            if (bc->combMode && !bc->combSequential) {
                                saveCombProgress(combProgressFile(ph->gpuId),
                                    visited, bc->combSlotsCount,
                                    bc->combCurrentPass, bc->combCycleNum,
                                    bc->combBaseOffset, bc->combSlots,
                                    bc->combSlotOrder, totalJumps,
                                    gZoneOrder, gZoneCursor, gZoneVisited);
                            }

                            // Mark previous slot as visited
                            int prevPass = bc->combCurrentPass - 1;
                            if (prevPass >= 0 && prevPass < bc->combSlotsCount) {
                                int orig = bc->combSlotOrder[prevPass];
                                if (orig >= 0 && orig < 2048) visited[orig] = 1;
                            }
                            int cur = (bc->combCurrentPass < bc->combSlotsCount)
                                      ? bc->combSlotOrder[bc->combCurrentPass] : 0;

                            // _ _ _ hex-_ _
                            std::string hexChunkS = combChunkStart.GetBase16();
                            std::string hexChunkE = combChunkEnd.GetBase16();
                            std::string hexSlotS  = bc->ksStart.GetBase16();
                            std::string hexSlotE  = bc->ksFinish.GetBase16();
                            std::string hexIlivS, hexIlivE;
                            if (bc->combInterleaveStep > 0.0) {
                                Int wStart; wStart.Set(&combChunkStart);
                                wStart.Add(&bc->combBaseOffset);
                                Int wEnd; wEnd.Set(&wStart);
                                Int wWidth; wWidth.Set(&bc->combCoverage);
                                Int ns2; ns2.SetInt32(bc->combSlotsCount);
                                wWidth.Mult(ns2.bits64[0]);
                                wEnd.Add(&wWidth); wEnd.SubOne();
                                if (wEnd.IsGreater(&combChunkEnd)) wEnd.Set(&combChunkEnd);
                                hexIlivS = wStart.GetBase16();
                                hexIlivE = wEnd.GetBase16();
                            } else {
                                hexIlivS = hexChunkS;
                                hexIlivE = hexChunkE;
                            }

                            // _ Draw slot table _
                            printf("\n");
                            for (int r = 0; r < ROWS; r++) {
                                printf("\033[2K");
                                for (int c = 0; c < COLS; c++) {
                                    int s = r * COLS + c;
                                    if (s == cur)          printf("@ ");
                                    else if (visited[s])   printf("# ");
                                    else                   printf(". ");
                                }
                                if ((r + 1) % 4 == 0) {
                                    int rowPct = (r + 1) * 100 / ROWS;
                                    printf(" %3d%%", rowPct);
                                }
                                putchar('\n');
                            }

                            // _ Status line _
                            printf("\033[2K[~] done:%-6d slot:%4d/%d  CYC:%d\n",
                                   totalJumps,
                                   bc->combCurrentPass,
                                   bc->combSlotsCount,
                                   bc->combCycleNum);

                            // _ Three hex lines _
                            printf("\033[2K[~] Chunk: 0x%s -> 0x%s\n",
                                   hexChunkS.c_str(), hexChunkE.c_str());
                            printf("\033[2K[~] Slot:  0x%s -> 0x%s\n",
                                   hexSlotS.c_str(),  hexSlotE.c_str());
                            printf("\033[2K[~] i-liv: 0x%s -> 0x%s\n",
                                   hexIlivS.c_str(),  hexIlivE.c_str());

                            // _ Zone bar _
                            printf("\033[2K[~] zones:[");
                            if (bc->combInterleaveStep <= 0.0) {
                                // _: _ _ _ _ _ _ 10 _ _
                                for (int z = 0; z < 10; z++) printf(" @");
                                printf(" ]  (All Chunk)\n");
                            } else {
                                // _: _ _ _ _
                                int curZoneIdx = 0;
                                {
                                    Int cSz; cSz.Set(&combChunkEnd);
                                    cSz.Sub(&combChunkStart); cSz.AddOne();
                                    Int ten2; ten2.SetInt32(10);
                                    Int zSz; zSz.Set(&cSz); zSz.Div(&ten2);
                                    if (!zSz.IsZero()) {
                                        Int tmp2; tmp2.Set(&bc->combBaseOffset);
                                        tmp2.Div(&zSz);
                                        uint64_t zi = tmp2.bits64[0];
                                        if (tmp2.bits64[1] != 0 || zi >= 10) zi = 9;
                                        curZoneIdx = (int)zi;
                                    }
                                }
                                if (bc->combInterleaveStep > 0.0
                                        && curZoneIdx >= 0 && curZoneIdx < 10)
                                    gZoneVisited[curZoneIdx] = 1;
                                for (int z = 0; z < 10; z++) {
                                    if (z == curZoneIdx)      printf(" @");
                                    else if (gZoneVisited[z]) printf(" _");
                                    else                      printf(" .");
                                }
                                printf(" ]\n");
                            }

                            // ROWS + 1(blank) + 1(status) + 3(hex) + 1(zones) = ROWS+6
                            printf("\033[%dA", ROWS + 6);
                            fflush(stdout);
                        }

                        printf("[~] %s %4d/%-4d  %3d%%  pos:0x%s   \r",
                               jtype,
                               bc->combCurrentPass, bc->combSlotsCount, pct,
                               bc->ksStart.GetBase16().c_str());
                        fflush(stdout);
                        combSlotT0 = Timer::get_tick();
                        // Reset time accumulators so speed display is per-slot
                        t_Paused = 0.0;
                        t0 = Timer::get_tick();
                        break;  // re-init GPU with new ksStart/ksFinish
                    }
                    break;
                }
            }

            ttot = Timer::get_tick() - t0 + t_Paused;

            if (backupMode && !bc->combMode && idxcount % 60 == 0) {
                saveBackup(idxcount, ttot, ph->gpuId);
            }

            Int keys_done;
            keys_done.SetInt32(idxcount);
            keys_done.Mult(numThreadsGPU);
            keys_done.Mult(STEP_SIZE);
            
            // In COMB mode the slot coverage is small; don't stop on keys_done.
            // COMB has its own termination (combDone) after all slots visited.
            if ((!bc->combMode || bc->combSequential) &&
                    keys_done.IsGreaterOrEqual(&total_keyspace)) {
                endOfSearch = true;
                printf("\n[+] Search range completed.\r");
                fflush(stdout);
            }

            Int keycount;
            keycount.SetInt32(idxcount - 1);
            keycount.Mult(STEP_SIZE);

            for (int i = 0; i < (int)found.size() && !endOfSearch; i++) {
                ITEM it = found[i];
                Int part_key;
                part_key.Set(&stepThread);
                part_key.Mult(it.thId);
    
                Int privkey;
                privkey.Set(&bc->ksStart);
                privkey.Add(&part_key);
                privkey.Add(&keycount);
            
                if (this->searchType == PUBKEY) {
                    Int k(&privkey);
                    if (it.incr < 0) {
                        k.Add((uint64_t)(-it.incr));
                        k.Neg();
                        k.Add(&secp->order);
                    } else {
                        k.Add((uint64_t)it.incr);
                    }
                    output(this->inputAddresses[0], secp->GetPrivAddress(true, k), k.GetBase16());
                    nbFoundKey++;
                    updateFound();
                } else {
                    checkAddr(*(address_t*)(it.hash), it.hash, privkey, it.incr, it.endo, it.mode);
                }
            }

            keys_n = 1ULL * STEP_SIZE * numThreadsGPU * idxcount;
            PrintStats(keys_n, ttot, total_keyspace);
        } 

        
        
        
        t_Paused += (Timer::get_tick() - t0);

    } 

    if (backupMode && !bc->combMode) {
        double final_ttot = Timer::get_tick() - t0 + t_Paused;
        saveBackup(idxcount, final_ttot, ph->gpuId);
    }
    
    ph->isRunning = false;
}

void VanitySearch::PrintStats(uint64_t keys_n, double ttot, const Int& total_keyspace) {
    
    if (endOfSearch) {
        return;
    }

    if (ttot < 0.1) return;

    double speed = (keys_n > 0) ? static_cast<double>(keys_n) / ttot / 1000000.0 : 0.0;
    double log_keys = (keys_n > 0) ? log2(static_cast<double>(keys_n)) : 0.0;
    
    double total_space_double = total_keyspace.ToDouble();
    double prob = (keys_n > 0) ? (static_cast<double>(keys_n) / total_space_double) : 0.0;
    
    double keys_per_sec = speed * 1000000.0;
    double time_to_50_percent = (keys_per_sec > 0) ? (0.5 * total_space_double) / keys_per_sec : std::numeric_limits<double>::infinity();

    string time_str_50 = format_time_long(time_to_50_percent);

    if (false) {
         printf("[+] [GPU %.2f Mkey/s][Total 2^%.2f][Prob %.1e%%][50%% in %s] \r",
               speed, log_keys, prob * 100.0, time_str_50.c_str());
    } else {
        
        char combTag[128] = "";
        if (bc && bc->combMode) {
            if (bc->combSequential && bc->combInterleaveStep == 0.0) {
                // SWEEP sequential: show real % progress
                double pct = prob * 100.0; if(pct>100.0)pct=100.0;
                snprintf(combTag,sizeof(combTag)," [SWEEP %.1f%%|%.0fs]",
                         pct, ttot<0?0.0:ttot);
            } else {
                const char* ct = bc->combSequential ? "SWEEP" : "COMB";
                double jumpSec = bc->combJumpMinutes * 60.0;
                char tstr[20];
                if (jumpSec > 0.0) {
                    double rem = jumpSec - ttot;
                    if(rem<0)rem=0;
                    snprintf(tstr,20,"|%02d:%02d",(int)(rem/60),(int)rem%60);
                } else {
                    snprintf(tstr,20,"|%.0fs",ttot<0?0.0:ttot);
                }
                if (bc->combInterleaveStep > 0.0) {
                    // Random jump mode: show cycle number
                    snprintf(combTag,sizeof(combTag)," [%s %d/%d%s|CYC:%d]",
                             ct,bc->combCurrentPass+1,bc->combSlotsCount,
                             tstr, bc->combCycleNum);
                } else {
                    snprintf(combTag,sizeof(combTag)," [%s %d/%d%s]",
                             ct,bc->combCurrentPass+1,bc->combSlotsCount,tstr);
                }
            }
        }
        // Sequential (SWEEP) bar _ uses prob already computed
        if (bc && bc->combMode && bc->combSequential && bc->combInterleaveStep == 0.0) {
            const int BAR_W = 100;
            char seqBar[102]; memset(seqBar,'.',BAR_W); seqBar[BAR_W]=0;
            double p = prob; if(p>1.0)p=1.0; if(p<0.0)p=0.0;
            int bpos = (int)(p * BAR_W);
            if(bpos >= BAR_W) bpos = BAR_W-1;
            // Build colored bar char by char
            char pLine[104]; memset(pLine,' ',BAR_W); pLine[BAR_W]=0;
            char pStr[8]; snprintf(pStr,8,"%.1f%%",p*100.0);
            int pl=(int)strlen(pStr), pp2=bpos-pl/2;
            if(pp2<0)pp2=0; if(pp2+pl>BAR_W)pp2=BAR_W-pl;
            memcpy(pLine+pp2,pStr,pl);
            printf("\n" "[~] %s" "\n" "[~] [", pLine);
            for(int i=0;i<BAR_W;i++) {
                if(i<bpos)      printf("|");
                else if(i==bpos) printf("#");
                else             printf(".");
            }
            printf("]" "\n"
                   "[~] SWEEP sequential" "\033[3A");
        }

        printf("[+] [GPU %.2f Mkey/s]" "[Total 2^%.2f][Prob %.2f%%][50%% in %s]" "%s" " \r",
               speed, log_keys, prob * 100.0, time_str_50.c_str(), combTag);
    }

    fflush(stdout);
}

void VanitySearch::saveBackup(int idxcount, double t_Paused, int gpuid) {
	std::string filename = "schedule_gpu" + std::to_string(gpuid) + ".dat";
	std::ofstream outFile(filename, std::ios::binary);
	if (outFile) {
		outFile.write(reinterpret_cast<const char*>(&idxcount), sizeof(idxcount));
		outFile.write(reinterpret_cast<const char*>(&t_Paused), sizeof(t_Paused));
		outFile.close();
	}
	else {
		std::cerr << "Error opening file for writing: " << filename << "\n";
	}
}

bool VanitySearch::isAlive(TH_PARAM * p) {

	bool isAlive = true;
	int total = numGPUs;
	for (int i = 0; i < total; i++)
		isAlive = isAlive && p[i].isRunning;

	return isAlive;
}

bool VanitySearch::hasStarted(TH_PARAM * p) {

	bool hasStarted = true;
	int total = numGPUs;
	for (int i = 0; i < total; i++)
		hasStarted = hasStarted && p[i].hasStarted;

	return hasStarted;
}

uint64_t VanitySearch::getGPUCount() {

	uint64_t count = 0;
	for (int i = 0; i < numGPUs; i++) {
		count += counters[i];
	}
	return count;
}

void VanitySearch::saveProgress(TH_PARAM* p, Int& lastSaveKey, BITCRACK_PARAM* bc) {

	Int lowerKey;
	lowerKey.Set(&p[0].THnextKey);

	int total = numGPUs;
	for (int i = 0; i < total; i++) {
		if (p[i].THnextKey.IsLower(&lowerKey))
			lowerKey.Set(&p[i].THnextKey);
	}

	if (lowerKey.IsLowerOrEqual(&lastSaveKey)) return;
	lastSaveKey.Set(&lowerKey);
}

void VanitySearch::Search(std::vector<int> gpuId, std::vector<int> gridSize) {
	endOfSearch = false;
	numGPUs = (int)gpuId.size();
	nbFoundKey = 0;

	memset(counters, 0, sizeof(counters));	

	TH_PARAM* params = (TH_PARAM*)malloc(numGPUs * sizeof(TH_PARAM));
	memset(params, 0, numGPUs * sizeof(TH_PARAM));
	
	
	std::thread* threads = new std::thread[numGPUs];

#ifdef WIN64
	ghMutex = CreateMutex(NULL, FALSE, NULL);
	mutex = CreateMutex(NULL, FALSE, NULL);
#else
	ghMutex = PTHREAD_MUTEX_INITIALIZER;
	mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

	
	for (int i = 0; i < numGPUs; i++) {
		params[i].obj = this;
		params[i].threadId = i;
		params[i].isRunning = true;
		params[i].gpuId = gpuId[i];
		
		params[i].gridSizeX = gridSize[i*2];
		params[i].gridSizeY = gridSize[i*2+1];
		params[i].THnextKey.Set(&bc->ksNext);
		
		threads[i] = std::thread(_FindKeyGPU, params + i);
	}

    
	while (!hasStarted(params)) {
		Timer::SleepMillis(500);
	}

    
	while (!endOfSearch) {
		Timer::SleepMillis(100);
	}

    
    
    for (int i = 0; i < numGPUs; i++) {
        if (threads[i].joinable()) {
            threads[i].join();
        }
    }
	
	if (params != nullptr) {
		free(params);
	}
    
    delete[] threads;
}

string VanitySearch::GetHex(vector<unsigned char> &buffer) {

	string ret;

	char tmp[128];
	for (int i = 0; i < (int)buffer.size(); i++) {
		sprintf(tmp, "%02hhX", buffer[i]);
		ret.append(tmp);
	}

	return ret;
}
