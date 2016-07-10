/***********************************************************************************************************************
 * Copyright (C) 2016 Andrew Zonenberg and contributors                                                                *
 *                                                                                                                     *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU Lesser General   *
 * Public License as published by the Free Software Foundation; either version 2.1 of the License, or (at your option) *
 * any later version.                                                                                                  *
 *                                                                                                                     *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied  *
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for     *
 * more details.                                                                                                       *
 *                                                                                                                     *
 * You should have received a copy of the GNU Lesser General Public License along with this program; if not, you may   *
 * find one here:                                                                                                      *
 * https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt                                                              *
 * or you may search the http://www.gnu.org website for the version 2.1 license, or you may write to the Free Software *
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA                                      *
 **********************************************************************************************************************/

#include <string.h>
#include "gp4prog.h"

using namespace std;

void ShowUsage();
void ShowVersion();

vector<uint8_t> ReadBitstream(string fname);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Entry point

int main(int argc, char* argv[])
{
	LogSink::Severity console_verbosity = LogSink::NOTICE;

	string fname;

	double voltage = 0.0;
	vector<int> nets;

	//Parse command-line arguments
	for(int i=1; i<argc; i++)
	{
		string s(argv[i]);
		
		if(s == "--help")
		{
			ShowUsage();
			return 0;
		}
		else if(s == "--version")
		{
			ShowVersion();
			return 0;
		}
		else if(s == "--verbose")
		{
			console_verbosity = LogSink::VERBOSE;
		}
		else if(s == "-q" || s == "--quiet")
		{
			if(console_verbosity == LogSink::NOTICE)
				console_verbosity = LogSink::WARNING;
			else if(console_verbosity == LogSink::WARNING)
				console_verbosity = LogSink::ERROR;
		}
		else if(s == "-v" || s == "--voltage")
		{
			if(i+1 < argc)
			{
				char *endptr;
				voltage = strtod(argv[++i], &endptr);
				if(*endptr)
				{
					printf("--voltage must be a decimal value\n");
					return 1;
				}
				if(!(voltage == 0.0 || (voltage >= 1.71 && voltage <= 5.5)))
				{
					printf("--voltage %.3g outside of valid range\n", voltage);
					return 1;
				}
			}
			else
			{
				printf("--voltage requires an argument\n");
				return 1;
			}
		}
		else if(s == "-n" || s == "--nets")
		{
			if(i+1 < argc)
			{
				char *arg = argv[++i];
				do {
					long net = strtol(arg, &arg, 10);
					if(*arg && *arg != ',')
					{
						printf("--nets must be a comma-separate list of net numbers\n");
						return 1;
					}
					if(net < 1 || net > 20 || net == 11)
					{
						printf("--nets used with an invalid net %ld\n", net);
						return 1;
					}
					nets.push_back(net);
				} while(*arg++);
			}
			else
			{
				printf("--nets requires an argument\n");
				return 1;
			}
		}

		//assume it's the bitstream file if it's the first non-switch argument
		else if( (s[0] != '-') && (fname == "") )
			fname = s;
			
		else
		{
			printf("Unrecognized command-line argument \"%s\", use --help\n", s.c_str());
			return 1;
		}
	}

	//Set up logging
	g_log_sinks.emplace(g_log_sinks.begin(), new STDLogSink(console_verbosity));
	
	//Print header
	if(console_verbosity >= LogSink::NOTICE)
		ShowVersion();

	vector<uint8_t> bitstream;
	if(!fname.empty()) {
		bitstream = ReadBitstream(fname);
		if(bitstream.empty())
			return 1;
	}

	if(!bitstream.empty() && voltage == 0.0) {
		LogError("Vdd must be provided for programming\n");
		return 1;
	}

	//Set up libusb
	USBSetup();
	
	// Try opening the board in "orange" mode
	LogNotice("\nSearching for programmer\n");
	hdevice hdev = OpenDevice(0x0f0f, 0x0006);
	if(!hdev) {
		// Try opening the board in "white" mode
 		hdev = OpenDevice(0x0f0f, 0x8006);
		if(!hdev) {
			LogError("No device found, giving up\n");
			return 1;
		}

		// Change the board into "orange" mode
		LogVerbose("Switching board into programmer mode\n");
		SwitchMode(hdev);

		// Takes a while to switch and re-enumerate
		usleep(1200 * 1000);

		// Try opening the board in "orange" mode again
		hdev = OpenDevice(0x0f0f, 0x0006);
		if(!hdev) {
			LogError("Could not switch mode, giving up\n");
			return 1;
		}
	}
	
	//Get string descriptors
	string name = GetStringDescriptor(hdev, 1);			//board name
	string vendor = GetStringDescriptor(hdev, 2);		//manufacturer
	LogNotice("Found: %s %s\n", vendor.c_str(), name.c_str());
	//string 0x80 is 02 03 for this board... what does that mean? firmware rev or something?
	//it's read by emulator during startup but no "2" and "3" are printed anywhere...
	
	//Light up the status LED
	SetStatusLED(hdev, 1);
	
	//Select part (no other parts supported yet).
	//TODO: see if we can enumerate what's plugged in / check the bitstream supplied
	LogNotice("Selecting part SLG46620V\n");
	SetPart(hdev, SLG46620V);

	//Wipe I/O config to floating
	LogNotice("Resetting I/O configuration\n");
	SetSiggenStatus(hdev, 1, SIGGEN_STOP);
	IOConfig config;
	SetIOConfig(hdev, config);

	if(bitstream.empty())
	{
		LogWarning("No bitstream provided, keeping the board in reset state\n");
	}
	else
	{
		//Configure the signal generator for Vdd
		LogNotice("Setting Vdd=%.3gV\n", voltage);
		ConfigureSiggen(hdev, 1, voltage);
		SetSiggenStatus(hdev, 1, SIGGEN_START);

		//Load bitstream
		LogNotice("Loading bitstream into SRAM\n");
		LoadBitstream(hdev, bitstream);
		
		//Set the I/O configuration on the test points
		//Expects a bitstream that does assign TP4=TP3;
		LogNotice("Setting I/O configuration\n");
		for(int net : nets) {
			// Note: for unknown reasons, the net has to be driven (e.g. weakly) for the LED to become active.
			// For unknown reasons, the vendor tool does not seem to suffer from this issue, and it
			// lets the LED light up even with all pins as TP_FLOAT.
			config.driverConfigs[net] = TP_PULLDOWN;
			config.ledEnabled[net] = true;
			config.expansionEnabled[net] = true;
		}
		SetIOConfig(hdev, config);
	}

	//Done
	LogNotice("Done\n");
	SetStatusLED(hdev, 0);
	
	USBCleanup(hdev);
	return 0;
}

void ShowUsage()
{
	printf(//                                                                               v 80th column
		"Usage: gp4prog foo.txt\n"
		"    -q, --quiet\n"
		"        Causes only warnings and errors to be written to the console.\n"
		"        Specify twice to also silence warnings.\n"
		"    -v, --voltage        <voltage>\n"
		"        Adjusts Vdd to the specified value in volts (0V to 5.5V), ±70mV.\n"
		"    -n, --nets           <nets>\n"
		"        For every test point in the specified comma-separated list:\n"
		"          * enables a non-inverted LED, if any;\n"
		"          * enables expansion connector passthrough.\n");
}

void ShowVersion()
{
	printf(
		"Greenpak4 programmer by Andrew D. Zonenberg and whitequark.\n"
		"\n"
		"License: LGPL v2.1+\n"
		"This is free software: you are free to change and redistribute it.\n"
		"There is NO WARRANTY, to the extent permitted by law.\n");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Bitstream reader

vector<uint8_t> ReadBitstream(string fname)
{	
	//Open the file
	FILE* fp = fopen(fname.c_str(), "r");
	if(!fp)
	{
		LogError("Couldn't open %s for reading\n", fname.c_str());
		return {};
	}
	
	char signature[64];
	fgets(signature, sizeof(signature), fp);
	if(strcmp(signature, "index\t\tvalue\t\tcomment\n")) 
	{
		LogError("%s is not a GreenPAK bitstream", fname.c_str());
		return {};
	}

	vector<uint8_t> bitstream;
	while(!feof(fp))
	{
		int index;
		int value;
		fscanf(fp, "%d\t\t%d\t\t//\n", &index, &value);

		int byteindex = index / 8;
		if(byteindex < 0 || (value != 0 && value != 1))
		{
			LogError("%s contains a malformed GreenPAK bitstream\n", fname.c_str());
			return {};
		}

		if(byteindex >= (int)bitstream.size())
			bitstream.resize(byteindex + 1, 0);
		bitstream[byteindex] |= (value << (index % 8));
	}

	fclose(fp);

	return bitstream;
}