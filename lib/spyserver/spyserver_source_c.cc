/* -*- c++ -*- */
/*
 * Copyright 2018 Lucas Teske <lucas@teske.com.br>
 *   Based on Youssef Touil (youssef@live.com) C# implementation.
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>
#include <algorithm>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/predef/other/endian.h>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <gnuradio/io_signature.h>

#include "spyserver_source_c.h"
#include "spyserver_protocol.h"

#include "arg_helpers.h"

using namespace boost::assign;

spyserver_source_c_sptr make_spyserver_source_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new spyserver_source_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr::block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 0;  // mininum number of input streams
static const int MAX_IN = 0;  // maximum number of input streams
static const int MIN_OUT = 1; // minimum number of output streams
static const int MAX_OUT = 1; // maximum number of output streams

/*
 * The private constructor
 */
spyserver_source_c::spyserver_source_c (const std::string &args)
  : gr::sync_block ("spyserver_source_c",
        gr::io_signature::make(MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr::io_signature::make(MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    terminated(false),
    streaming(false),
    got_device_info(false),
    receiver_thread(NULL),
    header_data(new uint8_t[sizeof(MessageHeader)]),
    body_buffer(NULL),
    body_buffer_length(0),
    parser_position(0),
    last_sequence_number(0),

    streaming_mode(STREAM_MODE_IQ_ONLY),
    _sample_rate(0),
    _center_freq(0),
    _gain(0),
    _digitalGain(0)
{
  dict_t dict = params_to_dict(args);

  ip = "";
  port = 0;
  if (dict.count("ip"))
  {
    ip = boost::lexical_cast<std::string>( dict["ip"] );
  }
  else if (dict.count("host"))
  {
    ip = boost::lexical_cast<std::string>( dict["host"] );
  }
  else if (dict.count("spyserver"))
  {
    std::vector<std::string> spyserver;
    boost::split(spyserver,dict["spyserver"],boost::is_any_of(":"));
    ip=spyserver[0];
    if(spyserver.size()==2)
      port=boost::lexical_cast<int>( spyserver[1] );
    else
      port=5555;
  }
  if (ip == "")
  {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "You should defined an IP to connect." );
  }

  if (dict.count("port"))
  {
    port = boost::lexical_cast<int>( dict["port"] );
  }
  else
  {
    if (port == 0)
      port = 5555;
  }

  if (dict.count("bits"))
    bits = boost::lexical_cast<int>( dict["bits"] );
  else
    bits = 8;
  if((bits!=8)&&(bits!=16))
    bits = 8;

  std::cerr << "SpyServer(" << ip << ", " << port << ")" << std::endl;
  client = tcp_client(ip, port);

  connect();

  _fifo = new boost::circular_buffer<gr_complex>(1024*1024*2);
  if (!_fifo) {
    throw std::runtime_error( std::string(__FUNCTION__) + " " +
                              "Failed to allocate a sample FIFO!" );
  }
  std::cerr << "SpyServer: Ready" << std::endl;
    std::cerr << "SpyServer: Starting Streaming" << std::endl;
    streaming = true;
    down_stream_bytes = 0;
    set_stream_state();
}

// const std::string &spyserver_source_c::getName() {
//   switch (device_info.DeviceType) {
//   case DEVICE_INVALID:
//     return spyserver_source_c::NameNoDevice;
//   case DEVICE_AIRSPY_ONE:
//     return spyserver_source_c::NameAirspyOne;
//   case DEVICE_AIRSPY_HF:
//     return spyserver_source_c::NameAirspyHF;
//   case DEVICE_RTLSDR:
//     return spyserver_source_c::NameRTLSDR;
//   default:
//     return spyserver_source_c::NameUnknown;
//   }
// }

void spyserver_source_c::connect()
{
  bool hasError = false;
  if (receiver_thread != NULL) {
    return;
  }

  std::cerr << "SpyServer: Trying to connect" << std::endl;
  client.connect_conn();
  is_connected = true;
  std::cerr << "SpyServer: Connected" << std::endl;

  say_hello();
  cleanup();

  terminated = false;
  got_sync_info = false;
  got_device_info = false;

  std::exception error;

  receiver_thread  = new std::thread(&spyserver_source_c::thread_loop, this);

  for (int i=0; i<1000 && !hasError; i++) {
    if (got_device_info) {
      if (device_info.DeviceType == DEVICE_INVALID) {
        error = std::runtime_error( std::string(__FUNCTION__) + " " + "Server is up but no device is available");
        hasError = true;
        break;
      }

      if (got_sync_info) {
        std::cerr << "SpyServer: Got sync Info" << std::endl;
        on_connect();
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  disconnect();
  if (hasError) {
    throw error;
  }

  throw std::runtime_error( std::string(__FUNCTION__) + " " + "Server didn't send the device capability and synchronization info.");
}

void spyserver_source_c::disconnect()
{
  terminated = true;
  if (is_connected) {
    client.close_conn();
  }

  if (receiver_thread != NULL) {
    receiver_thread->join();
    receiver_thread = NULL;
  }

  cleanup();
}


void spyserver_source_c::on_connect()
{
  set_setting(SETTING_STREAMING_MODE, { streaming_mode });
  set_setting(SETTING_IQ_FORMAT, { (bits==8)?STREAM_FORMAT_UINT8:STREAM_FORMAT_INT16 });
  // set_setting(SETTING_FFT_FORMAT, { STREAM_FORMAT_UINT8 });
  //set_setting(SETTING_FFT_DISPLAY_PIXELS, { displayPixels });
  //set_setting(SETTING_FFT_DB_OFFSET, { fftOffset });
  //set_setting(SETTING_FFT_DB_RANGE, { fftRange });
  //device_info.MaximumSampleRate
  //availableSampleRates
  std::cerr << "SpyServer: Maximum Sample Rate: " << device_info.MaximumSampleRate << std::endl;
  for (unsigned int i = device_info.MinimumIQDecimation; i<=device_info.DecimationStageCount; i++) {
    uint32_t sr = device_info.MaximumSampleRate / (1 << i);
    _sample_rates.push_back( std::pair<double, uint32_t>((double)sr, i ) );
  }
  std::sort(_sample_rates.begin(), _sample_rates.end());
}

bool spyserver_source_c::set_setting(uint32_t settingType, std::vector<uint32_t> params) {
  std::vector<uint8_t> argBytes;
  if (params.size() > 0) {
    argBytes = std::vector<uint8_t>(sizeof(SettingType) + params.size() * sizeof(uint32_t));
    uint8_t *settingBytes = (uint8_t *) &settingType;
    for (unsigned int i=0; i<sizeof(uint32_t); i++) {
      argBytes[i] = settingBytes[i];
    }

    std::memcpy(&argBytes[0]+sizeof(uint32_t), &params[0], sizeof(uint32_t) * params.size());
  } else {
    argBytes = std::vector<uint8_t>();
  }

  return send_command(CMD_SET_SETTING, argBytes);
}

bool spyserver_source_c::say_hello() {
  const uint8_t *protocolVersionBytes = (const uint8_t *) &ProtocolVersion;
  const uint8_t *softwareVersionBytes = (const uint8_t *) SoftwareID.c_str();
  std::vector<uint8_t> args = std::vector<uint8_t>(sizeof(ProtocolVersion) + SoftwareID.size());

  std::memcpy(&args[0], protocolVersionBytes, sizeof(ProtocolVersion));
  std::memcpy(&args[0] + sizeof(ProtocolVersion), softwareVersionBytes, SoftwareID.size());

  return send_command(CMD_HELLO, args);
}

void spyserver_source_c::cleanup() {
    device_info.DeviceType = 0;
    device_info.DeviceSerial = 0;
    device_info.DecimationStageCount = 0;
    device_info.GainStageCount = 0;
    device_info.MaximumSampleRate = 0;
    device_info.MaximumBandwidth = 0;
    device_info.MaximumGainIndex = 0;
    device_info.MinimumFrequency = 0;
    device_info.MaximumFrequency = 0;

    _gain = 0;
    _digitalGain = 0;
    //displayCenterFrequency = 0;
    //device_center_frequency = 0;
    //displayDecimationStageCount = 0;
    //channel_decimation_stage_count = 0;
    //minimum_tunable_frequency = 0;
    //maximum_tunable_frequency = 0;
    can_control = false;
    got_device_info = false;
    got_sync_info = false;

    last_sequence_number = ((uint32_t)-1);
    dropped_buffers = 0;
    down_stream_bytes = 0;

    parser_phase = AcquiringHeader;
    parser_position = 0;

    streaming = false;
    terminated = true;
}


void spyserver_source_c::thread_loop() {
  parser_phase = AcquiringHeader;
  parser_position = 0;

  try {
    while(!terminated) {
      if (terminated) {
        break;
      }
      uint32_t availableData = client.available_data();
      if (availableData > 0) {
        availableData = availableData > BufferSize ? BufferSize : availableData;
        client.receive_data(buffer, availableData);
        parse_message(buffer, availableData);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (std::exception &e) {
    std::cerr << "SpyServer: Error on ThreadLoop: " << e.what() << std::endl;
  }
  if (body_buffer != NULL) {
    delete[] body_buffer;
    body_buffer = NULL;
  }

  cleanup();
}

void spyserver_source_c::parse_message(char *buffer, uint32_t len) {
  down_stream_bytes++;

  int consumed;
  while (len > 0 && !terminated) {
    if (parser_phase == AcquiringHeader) {
      while (parser_phase == AcquiringHeader && len > 0) {
        consumed = parse_header(buffer, len);
        buffer += consumed;
        len -= consumed;
      }

      if (parser_phase == ReadingData) {
        uint8_t client_major = (SPYSERVER_PROTOCOL_VERSION >> 24) & 0xFF;
        uint8_t client_minor = (SPYSERVER_PROTOCOL_VERSION >> 16) & 0xFF;

        uint8_t server_major = (header.ProtocolID >> 24) & 0xFF;
        uint8_t server_minor = (header.ProtocolID >> 16) & 0xFF;
        //uint16_t server_build = (header.ProtocolID & 0xFFFF);

        if (client_major != server_major || client_minor != server_minor) {
          throw std::runtime_error( std::string(__FUNCTION__) + " " + "Server is running an unsupported protocol version.");
        }

        if (header.BodySize > SPYSERVER_MAX_MESSAGE_BODY_SIZE) {
          throw std::runtime_error( std::string(__FUNCTION__) + " " + "The server is probably buggy.");
        }

        if (body_buffer == NULL || body_buffer_length < header.BodySize) {
          if (body_buffer != NULL) {
            delete[] body_buffer;
          }

          body_buffer = new uint8_t[header.BodySize];
        }
      }
    }

    if (parser_phase == ReadingData) {
      consumed = parse_body(buffer, len);
      buffer += consumed;
      len -= consumed;

      if (parser_phase == AcquiringHeader) {
        if (header.MessageType != MSG_TYPE_DEVICE_INFO && header.MessageType != MSG_TYPE_CLIENT_SYNC) {
          int32_t gap = header.SequenceNumber - last_sequence_number - 1;
          last_sequence_number = header.SequenceNumber;
          dropped_buffers += gap;
          if (gap > 0) {
            std::cerr << "SpyServer: Lost " << gap << " frames from SpyServer!";
          }
        }
        handle_new_message();
      }
    }
  }
}

int spyserver_source_c::parse_header(char *buffer, uint32_t length) {
  auto consumed = 0;

  while (length > 0) {
    int to_write = std::min((uint32_t)(sizeof(MessageHeader) - parser_position), length);
    std::memcpy(&header + parser_position, buffer, to_write);
    length -= to_write;
    buffer += to_write;
    parser_position += to_write;
    consumed += to_write;
    if (parser_position == sizeof(MessageHeader)) {
      parser_position = 0;
      if (header.BodySize > 0) {
        parser_phase = ReadingData;
      }

      return consumed;
    }
  }

  return consumed;
}

int spyserver_source_c::parse_body(char* buffer, uint32_t length) {
  auto consumed = 0;

  while (length > 0) {
    int to_write = std::min((int) header.BodySize - parser_position, length);
    std::memcpy(body_buffer + parser_position, buffer, to_write);
    length -= to_write;
    buffer += to_write;
    parser_position += to_write;
    consumed += to_write;

    if (parser_position == header.BodySize) {
      parser_position = 0;
      parser_phase = AcquiringHeader;
      return consumed;
    }
  }

  return consumed;
}

bool spyserver_source_c::send_command(uint32_t cmd, std::vector<uint8_t> args) {
  if (!is_connected) {
    return false;
  }

  bool result;
  uint32_t headerLen = sizeof(CommandHeader);
  uint16_t argLen = args.size();
  uint8_t *buffer = new uint8_t[headerLen + argLen];

  CommandHeader header;
  header.CommandType = cmd;
  header.BodySize = argLen;

  for (uint32_t i=0; i<sizeof(CommandHeader); i++) {
    buffer[i] = ((uint8_t *)(&header))[i];
  }

  if (argLen > 0) {
    for (uint16_t i=0; i<argLen; i++) {
      buffer[i+headerLen] = args[i];
    }
  }

  try {
    client.send_data((char *)buffer, headerLen+argLen);
    result = true;
  } catch (std::exception &e) {
    result = false;
  }

  delete[] buffer;
  return result;
}

void spyserver_source_c::handle_new_message() {
  if (terminated) {
    return;
  }

  switch (header.MessageType) {
    case MSG_TYPE_DEVICE_INFO:
      process_device_info();
      break;
    case MSG_TYPE_CLIENT_SYNC:
      process_client_sync();
      break;
    case MSG_TYPE_UINT8_IQ:
      process_uint8_samples();
      break;
    case MSG_TYPE_INT16_IQ:
      process_int16_samples();
      break;
    case MSG_TYPE_FLOAT_IQ:
      process_float_samples();
      break;
    case MSG_TYPE_UINT8_FFT:
      process_uint8_fft();
      break;
    default:
      break;
  }
}

void spyserver_source_c::process_device_info() {
  std::memcpy(&device_info, body_buffer, sizeof(DeviceInfo));
  minimum_tunable_frequency = device_info.MinimumFrequency;
  maximum_tunable_frequency = device_info.MaximumFrequency;
  got_device_info = true;
}

void spyserver_source_c::process_client_sync() {
  ClientSync sync;
  std::memcpy(&sync, body_buffer, sizeof(ClientSync));

  can_control = sync.CanControl != 0;
  _gain = (double) sync.Gain;
  device_center_frequency = sync.DeviceCenterFrequency;
  channel_center_frequency = sync.IQCenterFrequency;
  _center_freq = (double) sync.IQCenterFrequency;

  switch (streaming_mode) {
  case STREAM_MODE_FFT_ONLY:
  case STREAM_MODE_FFT_IQ:
    minimum_tunable_frequency = sync.MinimumFFTCenterFrequency;
    maximum_tunable_frequency = sync.MaximumFFTCenterFrequency;
    break;
  case STREAM_MODE_IQ_ONLY:
    minimum_tunable_frequency = sync.MinimumIQCenterFrequency;
    maximum_tunable_frequency = sync.MaximumIQCenterFrequency;
    break;
  }

  got_sync_info = true;
}

void spyserver_source_c::process_uint8_samples() {
  size_t n_avail, to_copy, num_samples = (header.BodySize) / 2;
  _fifo_lock.lock();

  uint8_t *sample = (uint8_t *)body_buffer;

  n_avail = _fifo->capacity() - _fifo->size();
  to_copy = (n_avail < num_samples ? n_avail : num_samples);

  for (size_t i=0; i < to_copy; i++)
  {
    _fifo->push_back(gr_complex((*sample - 128.f) / 128.f, (*(sample+1) - 128.f) / 128.f));
    sample += 2;
  }
  _fifo_lock.unlock();
  if (to_copy) {
    _samp_avail.notify_one();
  }

  if (to_copy < num_samples)
    std::cerr << "O" << std::flush;
}

void spyserver_source_c::process_int16_samples() {
  size_t n_avail, to_copy, num_samples = (header.BodySize / 2) / 2;

  _fifo_lock.lock();

  int16_t *sample = (int16_t *)body_buffer;

  n_avail = _fifo->capacity() - _fifo->size();
  to_copy = (n_avail < num_samples ? n_avail : num_samples);

  for (size_t i=0; i < to_copy; i++)
  {
    _fifo->push_back(gr_complex(*sample / 32768.f, *(sample+1) / 32768.f));
    sample += 2;
  }
  _fifo_lock.unlock();
  if (to_copy) {
    _samp_avail.notify_one();
  }

  if (to_copy < num_samples)
    std::cerr << "O" << std::flush;
}

void spyserver_source_c::process_float_samples() {
  size_t n_avail, to_copy, num_samples = (header.BodySize / 4) / 2;
  _fifo_lock.lock();

  float *sample = (float *)body_buffer;

  n_avail = _fifo->capacity() - _fifo->size();
  to_copy = (n_avail < num_samples ? n_avail : num_samples);

  for (size_t i=0; i < to_copy; i++)
  {
    _fifo->push_back(gr_complex(*sample, *(sample+1)));
    sample += 2;
  }
  _fifo_lock.unlock();
  if (to_copy) {
    _samp_avail.notify_one();
  }
}

void spyserver_source_c::set_stream_state() {
  set_setting(SETTING_STREAMING_ENABLED, {(unsigned int)(streaming ? 1 : 0)});
}

double spyserver_source_c::set_sample_rate(double sampleRate) {
  if (sampleRate <= 0xFFFFFFFF) {
    std::cerr << "SpyServer: Setting sample rate to " << sampleRate << std::endl;
    for (unsigned int i=0; i<_sample_rates.size(); i++) {
      if (_sample_rates[i].first == sampleRate) {
              channel_decimation_stage_count = _sample_rates[i].second;
              set_setting(SETTING_IQ_DECIMATION, {channel_decimation_stage_count});
              _sample_rate = sampleRate;
              return get_sample_rate();
      }
    }
  }
  std::cerr << "SpyServer: Sample rate not supported: " << sampleRate << std::endl;
  std::cerr << "SpyServer: Supported Sample Rates: " << std::endl;
  for (std::pair<double, uint32_t> sr: _sample_rates) {
    std::cerr << "SpyServer:   " << sr.first << std::endl;
  }

  throw std::runtime_error(boost::str( boost::format("Unsupported samplerate: %gM") % (sampleRate/1e6) ) );
}

double spyserver_source_c::set_center_freq(double centerFrequency, size_t chan) {
  if (centerFrequency <= 0xFFFFFFFF) {
    channel_center_frequency = (uint32_t) centerFrequency;
    set_setting(SETTING_IQ_FREQUENCY, {channel_center_frequency});
    return centerFrequency;
  }

  std::cerr << boost::format("Unsupported center frequency: %gM") % (centerFrequency/1e6) << std::endl;

  return this->get_center_freq(chan);
}

void spyserver_source_c::process_uint8_fft() {
  // TODO
  // // std::cerr << "UInt8 FFT Samples processing not implemented!!!" << std::endl;
}

/*
 * Our virtual destructor.
 */
spyserver_source_c::~spyserver_source_c ()
{
  if (streaming) {
    std::cerr << "SpyServer: Stopping Streaming" << std::endl;
    streaming = false;
    down_stream_bytes = 0;
    set_stream_state();
  }
  disconnect();
  if (_fifo)
  {
    delete _fifo;
    _fifo = NULL;
  }
  delete[] header_data;
  header_data = NULL;
}

bool spyserver_source_c::start()
{
  return true;
}

bool spyserver_source_c::stop()
{
  return true;
}

int spyserver_source_c::work( int noutput_items,
                        gr_vector_const_void_star &input_items,
                        gr_vector_void_star &output_items )
{
  gr_complex *out = (gr_complex *)output_items[0];

  if ( ! streaming )
    return WORK_DONE;

  boost::unique_lock<boost::mutex> lock(_fifo_lock);

  /* Wait until we have the requested number of samples */
  int n_samples_avail = _fifo->size();

  if (n_samples_avail < noutput_items) {
    for(int i = 0; i < noutput_items; ++i)
      out[i] = gr_complex(0,0);
    return noutput_items;
  }

  for(int i = 0; i < noutput_items; ++i) {
    out[i] = _fifo->at(0);
    _fifo->pop_front();
  }

  //std::cerr << "-" << std::flush;

  return noutput_items;
}

std::vector<std::string> spyserver_source_c::get_devices(bool fake)
{
  std::vector<std::string> devices;
  std::string label;
  if ( fake )
  {
    std::string args = "spyserver=0,host=localhost,port=5555";
    args += ",label='Spyserver Client'";
    devices.push_back( args );
  }

  return devices;
}

size_t spyserver_source_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t spyserver_source_c::get_sample_rates()
{
  osmosdr::meta_range_t range;
  for (size_t i = 0; i < _sample_rates.size(); i++)
    range += osmosdr::range_t( _sample_rates[i].first );

  return range;
}

double spyserver_source_c::get_sample_rate()
{
  return _sample_rate;
}

osmosdr::freq_range_t spyserver_source_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;
  range += osmosdr::range_t( minimum_tunable_frequency, maximum_tunable_frequency );

  return range;
}

double spyserver_source_c::get_center_freq( size_t chan )
{
  return _center_freq;
}

double spyserver_source_c::set_freq_corr( double ppm, size_t chan )
{
  return get_freq_corr( chan );
}

double spyserver_source_c::get_freq_corr( size_t chan )
{
  return 0;
}

std::vector<std::string> spyserver_source_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;
  if (can_control) {
    names += "LNA";
  }
  names += "Digital";

  return names;
}

osmosdr::gain_range_t spyserver_source_c::get_gain_range( size_t chan )
{
  return osmosdr::gain_range_t( 0, 16, 1 );
}

osmosdr::gain_range_t spyserver_source_c::get_gain_range( const std::string & name, size_t chan )
{
  if (name == "Digital") {
    return osmosdr::gain_range_t( 0, 1, 1 );
  }
  return get_gain_range(chan);
}

bool spyserver_source_c::set_gain_mode( bool automatic, size_t chan )
{
  return get_gain_mode(chan);
}

bool spyserver_source_c::get_gain_mode( size_t chan )
{
  return false;
}

double spyserver_source_c::set_gain( double gain, size_t chan )
{
  if (can_control) {
    _gain = gain;
    set_setting(SETTING_GAIN, {(uint32_t)gain});
  } else {
    std::cerr << "Spyserver: The server does not allow you to change the gains." << std::endl;
  }

  return _gain;
}

double spyserver_source_c::set_lna_gain( double gain, size_t chan)
{
  return set_gain(gain, chan);
}

double spyserver_source_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if (name == "Digital") {
    _digitalGain = gain;
    set_setting(SETTING_IQ_DIGITAL_GAIN, {((uint32_t)gain) * 0xFFFFFFFF});
    return _gain;
  }
  return set_gain(gain, chan);
}

double spyserver_source_c::get_gain( size_t chan )
{
  return chan == 0 ? _gain : _digitalGain;
  return _gain;
}

double spyserver_source_c::get_gain( const std::string & name, size_t chan )
{

  if (name == "Digital") {
    return _digitalGain;
  }
  return get_gain(chan);
}

double spyserver_source_c::set_mix_gain(double gain, size_t chan)
{
  return _gain;
}

double spyserver_source_c::set_if_gain(double gain, size_t chan)
{
  return _gain;
}

std::vector< std::string > spyserver_source_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string spyserver_source_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string spyserver_source_c::get_antenna( size_t chan )
{
  return "RX";
}

double spyserver_source_c::set_bandwidth( double bandwidth, size_t chan )
{
  return get_bandwidth( chan );
}

double spyserver_source_c::get_bandwidth( size_t chan )
{
  return _sample_rate;
}

osmosdr::freq_range_t spyserver_source_c::get_bandwidth_range( size_t chan )
{
  osmosdr::freq_range_t bandwidths;

  bandwidths += osmosdr::range_t( get_bandwidth( chan ) );

  return bandwidths;
}

void spyserver_source_c::set_biast( bool enabled ) {

}

bool spyserver_source_c::get_biast() {
  return false;
}
