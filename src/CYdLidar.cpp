#include "CYdLidar.h"
#include "common.h"
#include <map>
#include <regex>

using namespace std;
using namespace ydlidar;
using namespace impl;


/*-------------------------------------------------------------
						Constructor
-------------------------------------------------------------*/
CYdLidar::CYdLidar(): lidarPtr(nullptr) {
  m_SerialPort        = "";
  m_SerialBaudrate    = 230400;
  m_Intensities       = false;
  m_AutoReconnect     = false;
  m_MaxAngle          = 360.f;
  m_MinAngle          = 0.f;
  m_MaxRange          = 16.0;
  m_MinRange          = 0.08;
  m_SampleRate        = 5;
  m_ScanFrequency     = 7;
  m_AngleOffset       = 0.0;
  m_isAngleOffsetCorrected = false;
  m_isLRRAngleOffsetCorrected = false;
  m_LRRAngleOffset = 0.0;
  m_startRobotAngleOffset = false;
  m_GlassNoise        = true;
  m_SunNoise          = true;
  isScanning          = false;
  frequencyOffset     = 0.4;
  m_CalibrationFileName = "";
  m_AbnormalCheckCount  = 2;
  Major               = 0;
  Minjor              = 0;
  m_RobotLidarDifference = 0.0;
  m_IgnoreArray.clear();
  ini.SetUnicode();
  m_serial_number.clear();
  m_angle_threshold.resize(2 * MAXCHECKTIMES);
  m_angle_threshold[CHECK_ANGLE_MIN] = (225);
  m_angle_threshold[CHECK_ANGLE_MAX] = (270);
  m_angle_threshold[SIGMA_ANGLE_MIN] = (270);
  m_angle_threshold[SIGMA_ANGLE_MAX] = (295);
  m_angle_threshold[SIGN_ANGLE_MIN]  = (295);
  m_angle_threshold[SIGN_ANGLE_MIN]  = (315);
  m_angle_threshold[SURE_ANGLE_MIN]  = (315);
  m_angle_threshold[SURE_ANGLE_MAX]  = (335);

  check_queue_size.resize(MAXCHECKTIMES);
  auto_check_sum_queue.resize(MAXCHECKTIMES);
  auto_check_distance.resize(MAXCHECKTIMES);
  m_action_startup = false;
  has_check_flag = false;
  current_frequency = 0.0;
  last_frequency = 0.0;
  m_action_step = 0;
  m_action_state = 0;
  action_check_time = getTime();
  has_check_state = false;
  m_state  = NORMAL;
}

/*-------------------------------------------------------------
                    ~CYdLidar
-------------------------------------------------------------*/
CYdLidar::~CYdLidar() {
  disconnecting();
}

void CYdLidar::disconnecting() {
  if (lidarPtr) {
    lidarPtr->disconnect();
    delete lidarPtr;
    lidarPtr = nullptr;
  }

  isScanning = false;
}

//lidar pointer
YDlidarDriver *CYdLidar::getYdlidarDriver() {
  return lidarPtr;
}

//get zero angle offset value
float CYdLidar::getAngleOffset() const {
  return m_AngleOffset;
}

bool CYdLidar::isAngleOffetCorrected() const {
  return m_isAngleOffsetCorrected;
}

float CYdLidar::getRobotAngleOffset() const {
  return m_LRRAngleOffset;
}

void CYdLidar::setStartRobotAngleOffset() {
  m_startRobotAngleOffset = true;
}

bool CYdLidar::isRobotAngleOffsetCorrected() const {
	return m_isLRRAngleOffsetCorrected && !m_startRobotAngleOffset;
}
/*-------------------------------------------------------------
						doProcessSimple
-------------------------------------------------------------*/
bool  CYdLidar::doProcessSimple(LaserScan &outscan, bool &hardwareError) {
  hardwareError			= false;

  // Bound?
  if (!checkHardware()) {
    hardwareError = true;
    delay(1000 / m_ScanFrequency);
    return false;
  }

  node_info *nodes = new node_info[YDlidarDriver::MAX_SCAN_NODES];
  size_t   count = YDlidarDriver::MAX_SCAN_NODES;

  //line feature
  indices.clear();
  bearings.clear();
  range_data.clear();

  //wait Scan data:
  result_t op_result =  lidarPtr->grabScanData(nodes, count);

  // Fill in scan data:
  if (IS_OK(op_result)) {
    uint64_t tim_scan_start = nodes[0].stamp;
    uint64_t tim_scan_end   = nodes[count - 1].stamp;
    float range = 0.0;
    float intensity = 0.0;
    float angle = 0.0;
    float ori_angle = 0.0;
    LaserScan scan_msg;
    LaserPoint point;

    if (m_MaxAngle < m_MinAngle) {
      float temp = m_MinAngle;
      m_MinAngle = m_MaxAngle;
      m_MaxAngle = temp;
    }

    retSetData();

    for (int i = 0; i < count; i++) {

      ori_angle = (float)((nodes[i].ori_angle_q6_checkbit >>
                           LIDAR_RESP_MEASUREMENT_ANGLE_SHIFT) / 64.0f);
      angle = (float)((nodes[i].angle_q6_checkbit >>
                       LIDAR_RESP_MEASUREMENT_ANGLE_SHIFT) / 64.0f) +
              m_AngleOffset;
      range = (float)nodes[i].distance_q / 1000.f;

      handleScanData(ori_angle, range);

      if (angle > 360) {
        angle -= 360;
      } else if (angle < 0) {
        angle += 360;
      }

      uint8_t intensities = (uint8_t)(nodes[i].sync_quality >>
                                      LIDAR_RESP_MEASUREMENT_QUALITY_SHIFT);
      intensity = (float)intensities;

      if (m_GlassNoise && intensities == GLASSNOISEINTENSITY) {
        intensity = 0.0;
        range     = 0.0;
      }

      if (m_SunNoise && intensities == SUNNOISEINTENSITY) {
        intensity = 0.0;
        range     = 0.0;
      }

      if (range > m_MaxRange || range < m_MinRange) {
        range = 0.0;
      }

      if (angle >= m_MinAngle && angle <= m_MaxAngle) {
        point.angle = angle;
        point.distance = range;
        point.intensity = intensity;
        scan_msg.data.push_back(point);
      }

      if (range >= m_MinRange) {
        double feature_angle = angles::from_degrees(angle);
        bearings.push_back(feature_angle);
        indices.push_back(indices.size());
        range_data.ranges.push_back(range);
        range_data.xs.push_back(cos(feature_angle)*range);
        range_data.ys.push_back(sin(feature_angle)*range);
      }

      if (nodes[i].stamp < tim_scan_start) {
        tim_scan_start = nodes[i].stamp;
      }

      if (nodes[i].stamp > tim_scan_end) {
        tim_scan_end = nodes[i].stamp;
      }

    }

    handleRobotOffsetAngle();

    double scan_time = (tim_scan_end - tim_scan_start) / 1e9;
    scan_msg.system_time_stamp = tim_scan_start;
    scan_msg.config.min_angle = (m_MinAngle);
    scan_msg.config.max_angle = (m_MaxAngle);
    scan_msg.config.time_increment = scan_time / (double)count;
    scan_msg.config.scan_time = scan_time;
    scan_msg.config.min_range = m_MinRange;
    scan_msg.config.max_range = m_MaxRange;
    outscan = scan_msg;
    delete[] nodes;
    {
      current_frequency = 1.0 / scan_time;
      handleCheckData();
      OnEnter(current_frequency);
      last_frequency = current_frequency;
    }


    return true;

  } else {
    if (IS_FAIL(op_result)) {
      // Error? Retry connection
    }
  }

  delete[] nodes;
  return false;

}

void CYdLidar::handleRobotOffsetAngle() {
  if (m_startRobotAngleOffset) {
    line_feature_.setCachedRangeData(bearings, indices, range_data);
    std::vector<gline> glines;
    line_feature_.extractLines(glines);
    bool find_line_angle = false;
    gline max;
    max.distance = 0.0;
    double line_angle = 0.0;

    if (glines.size()) {
      max = glines[0];
    }

    for (std::vector<gline>::const_iterator it = glines.begin();
         it != glines.end(); ++it) {
      line_angle = M_PI_2 - it->angle;
      line_angle = angles::normalize_angle(line_angle);

      if (fabs(line_angle) < fabs(angles::from_degrees(m_RobotLidarDifference) -
                                  M_PI / 12)) {
        if (it->distance > 0.5 && it->distance > max.distance) {
          max = (*it);
          find_line_angle = true;
          line_angle -= angles::from_degrees(m_RobotLidarDifference);
          m_LRRAngleOffset = angles::to_degrees(line_angle);
        }
      }
    }

    if (find_line_angle) {
      saveRobotOffsetAngle();
    }
  }
}


/*-------------------------------------------------------------
            turnOn
-------------------------------------------------------------*/
bool  CYdLidar::turnOn() {
  if (!lidarPtr) {
    return false;
  }

  if (isScanning && lidarPtr->isscanning()) {
    return true;
  }

  // start scan...
  result_t op_result = lidarPtr->startScan();

  if (!IS_OK(op_result)) {
    op_result = lidarPtr->startScan();

    if (!IS_OK(op_result)) {
      lidarPtr->stop();
      fprintf(stderr, "[CYdLidar] Failed to start scan mode: %x\n", op_result);
      isScanning = false;
      return false;
    }
  }

  if (checkLidarAbnormal()) {
    lidarPtr->stop();
    fprintf(stderr,
            "[CYdLidar] Failed to turn on the Lidar, because the lidar is blocked or the lidar hardware is faulty.\n");
    isScanning = false;
    return false;
  }

  isScanning = true;
  lidarPtr->setAutoReconnect(m_AutoReconnect);
  printf("[YDLIDAR INFO] Now YDLIDAR is scanning ......\n");
  fflush(stdout);
  return true;
}

/*-------------------------------------------------------------
            turnOff
-------------------------------------------------------------*/
bool  CYdLidar::turnOff() {
  if (lidarPtr) {
    lidarPtr->stop();
  } else {
    return false;
  }

  if (isScanning) {
    printf("[YDLIDAR INFO] Now YDLIDAR Scanning has stopped ......\n");
  }

  isScanning = false;
  return true;
}

bool CYdLidar::checkLidarAbnormal() {
  node_info *nodes = new node_info[YDlidarDriver::MAX_SCAN_NODES];
  size_t   count = YDlidarDriver::MAX_SCAN_NODES;
  int check_abnormal_count = 0;

  if (m_AbnormalCheckCount < 2) {
    m_AbnormalCheckCount = 2;
  }

  result_t op_result = RESULT_FAIL;

  while (check_abnormal_count < m_AbnormalCheckCount) {
    //Ensure that the voltage is insufficient or the motor resistance is high, causing an abnormality.
    if (check_abnormal_count > 0) {
      delay(check_abnormal_count * 1000);
    }

    op_result =  lidarPtr->grabScanData(nodes, count);

    if (IS_OK(op_result)) {
      delete[] nodes;
      return false;
    }

    check_abnormal_count++;
  }

  delete[] nodes;
  return !IS_OK(op_result);
}

/** Returns true if the device is connected & operative */
bool CYdLidar::getDeviceHealth() {
  if (!lidarPtr) {
    return false;
  }

  lidarPtr->stop();
  result_t op_result;
  device_health healthinfo;
  printf("[YDLIDAR]:SDK Version: %s\n", YDlidarDriver::getSDKVersion().c_str());
  op_result = lidarPtr->getHealth(healthinfo);

  if (IS_OK(op_result)) {
    printf("[YDLIDAR]:Lidar running correctly ! The health status: %s\n",
           (int)healthinfo.status == 0 ? "good" : "bad");

    if (healthinfo.status == 2) {
      fprintf(stderr,
              "Error, Yd Lidar internal error detected. Please reboot the device to retry.\n");
      return false;
    } else {
      return true;
    }

  } else {
    fprintf(stderr, "Error, cannot retrieve Yd Lidar health code: %x\n", op_result);
    return false;
  }

}

bool CYdLidar::getDeviceInfo() {
  if (!lidarPtr) {
    return false;
  }

  device_info devinfo;
  result_t op_result = lidarPtr->getDeviceInfo(devinfo);

  if (!IS_OK(op_result)) {
    fprintf(stderr, "get Device Information Error\n");
    return false;
  }

  if (devinfo.model != YDlidarDriver::YDLIDAR_R2_SS_1 &&
      devinfo.model != YDlidarDriver::YDLIDAR_G4) {
    printf("[YDLIDAR INFO] Current SDK does not support current lidar models[%d]\n",
           devinfo.model);
    return false;
  }

  std::string model = "R2-SS-1";
  int m_samp_rate = 5;

  switch (devinfo.model) {
    case YDlidarDriver::YDLIDAR_G4:
      model = "G4";
      break;

    case YDlidarDriver::YDLIDAR_R2_SS_1:
      model = "R2-SS-1";
      m_samp_rate = 5;
      m_SampleRate = m_samp_rate;
      break;

    default:
      break;
  }

  Major = (uint8_t)(devinfo.firmware_version >> 8);
  Minjor = (uint8_t)(devinfo.firmware_version & 0xff);
  std::string serial_number;
  printf("[YDLIDAR] Connection established in [%s][%d]:\n"
         "Firmware version: %u.%u\n"
         "Hardware version: %u\n"
         "Model: %s\n"
         "Serial: ",
         m_SerialPort.c_str(),
         m_SerialBaudrate,
         Major,
         Minjor,
         (unsigned int)devinfo.hardware_version,
         model.c_str());

  for (int i = 0; i < 16; i++) {
    serial_number += format("%01X", devinfo.serialnum[i] & 0xff);
  }

  printf("%s\n", serial_number.c_str());
  m_serial_number = serial_number;
  std::regex
  rx("^2(\\d{3})(0\\d{1}|1[0-2])(0\\d{1}|[12]\\d{1}|3[01])(\\d{4})(\\d{4})$");
  std::smatch result;

  if (!regex_match(serial_number, result, rx)) {
    fprintf(stderr, "Invalid lidar serial number!!!\n");
    return false;
  }

  if (devinfo.model == YDlidarDriver::YDLIDAR_R2_SS_1) {
    checkCalibrationAngle(serial_number);
  } else {
    m_isAngleOffsetCorrected = true;
    checkSampleRate();
  }

  checkRobotOffsetAngleCorrected(serial_number);

  printf("[YDLIDAR INFO] Current Sampling Rate : %dK\n", m_SampleRate);
  checkScanFrequency();
  return true;
}


void CYdLidar::checkSampleRate() {
  sampling_rate _rate;
  int _samp_rate = 9;
  int try_count;
  result_t ans = lidarPtr->getSamplingRate(_rate);

  if (IS_OK(ans)) {
    switch (m_SampleRate) {
      case 4:
        _samp_rate = YDlidarDriver::YDLIDAR_RATE_4K;
        break;

      case 8:
        _samp_rate = YDlidarDriver::YDLIDAR_RATE_8K;
        break;

      case 9:
        _samp_rate = YDlidarDriver::YDLIDAR_RATE_9K;
        break;

      default:
        _samp_rate = _rate.rate;
        break;
    }

    while (_samp_rate != _rate.rate) {
      ans = lidarPtr->setSamplingRate(_rate);

      if (!IS_OK(ans)) {
        try_count++;

        if (try_count > 3) {
          break;
        }
      }
    }

    switch (_rate.rate) {
      case YDlidarDriver::YDLIDAR_RATE_4K:
        _samp_rate = 4;
        break;

      case YDlidarDriver::YDLIDAR_RATE_8K:
        _samp_rate = 8;
        break;

      case YDlidarDriver::YDLIDAR_RATE_9K:
        _samp_rate = 9;
        break;

      default:
        break;
    }
  }

  m_SampleRate = _samp_rate;

}

/*-------------------------------------------------------------
                        checkScanFrequency
-------------------------------------------------------------*/
bool CYdLidar::checkScanFrequency() {
  float frequency = 7.4f;
  scan_frequency _scan_frequency;
  float hz = 0;
  result_t ans = RESULT_FAIL;
  m_ScanFrequency += frequencyOffset;

  if (5.0 - frequencyOffset <= m_ScanFrequency &&
      m_ScanFrequency <= 12 + frequencyOffset) {
    ans = lidarPtr->getScanFrequency(_scan_frequency) ;

    if (IS_OK(ans)) {
      frequency = _scan_frequency.frequency / 100.f;
      hz = m_ScanFrequency - frequency;

      if (hz > 0) {
        while (hz > 0.95) {
          lidarPtr->setScanFrequencyAdd(_scan_frequency);
          hz = hz - 1.0;
        }

        while (hz > 0.09) {
          lidarPtr->setScanFrequencyAddMic(_scan_frequency);
          hz = hz - 0.1;
        }

        frequency = _scan_frequency.frequency / 100.0f;
      } else {
        while (hz < -0.95) {
          lidarPtr->setScanFrequencyDis(_scan_frequency);
          hz = hz + 1.0;
        }

        while (hz < -0.09) {
          lidarPtr->setScanFrequencyDisMic(_scan_frequency);
          hz = hz + 0.1;
        }

        frequency = _scan_frequency.frequency / 100.0f;
      }
    }
  } else {
    fprintf(stderr, "current scan frequency[%f] is out of range.",
            m_ScanFrequency - frequencyOffset);
  }

  ans = lidarPtr->getScanFrequency(_scan_frequency);

  if (IS_OK(ans)) {
    frequency = _scan_frequency.frequency / 100.0f;
    m_ScanFrequency = frequency;
  }

  m_ScanFrequency -= frequencyOffset;
  printf("[YDLIDAR INFO] Current Scan Frequency: %fHz\n", m_ScanFrequency);
  return true;
}

/*-------------------------------------------------------------
                        checkCalibrationAngle
-------------------------------------------------------------*/
void CYdLidar::checkCalibrationAngle(const std::string &serialNumber) {
  m_AngleOffset = 0.0;
  result_t ans;
  offset_angle angle;
  int retry = 0;
  m_isAngleOffsetCorrected = false;

  while (retry < 2 && (Major > 1 || (Major >= 1 && Minjor > 1))) {
    ans = lidarPtr->getZeroOffsetAngle(angle);

    if (IS_OK(ans)) {
      if (angle.angle > 720 || angle.angle < -720) {
        ans = lidarPtr->getZeroOffsetAngle(angle);

        if (!IS_OK(ans)) {
          continue;
          retry++;
        }
      }

      m_isAngleOffsetCorrected = (angle.angle != 720);
      m_AngleOffset = angle.angle / 4.0;
      printf("[YDLIDAR INFO] Successfully obtained the %s offset angle[%f] from the lidar[%s]\n"
             , m_isAngleOffsetCorrected ? "corrected" : "uncorrrected", m_AngleOffset,
             serialNumber.c_str());
      return;
    }

    retry++;
  }

  if (ydlidar::fileExists(m_CalibrationFileName)) {
    SI_Error rc = ini.LoadFile(m_CalibrationFileName.c_str());

    if (rc >= 0) {
      m_isAngleOffsetCorrected = true;
      double default_value = 179.6;
      m_AngleOffset = ini.GetDoubleValue("CALIBRATION", serialNumber.c_str(),
                                         default_value);

      if (fabs(m_AngleOffset - default_value) < 0.01) {
        m_isAngleOffsetCorrected = false;
        m_AngleOffset = 0.0;
      }

      printf("[YDLIDAR INFO] Successfully obtained the %s offset angle[%f] from the calibration file[%s]\n"
             , m_isAngleOffsetCorrected ? "corrected" : "uncorrrected", m_AngleOffset,
             m_CalibrationFileName.c_str());

    } else {
      printf("[YDLIDAR INFO] Failed to open calibration file[%s]\n",
             m_CalibrationFileName.c_str());
    }
  } else {
    printf("[YDLIDAR INFO] Calibration file[%s] does not exist\n",
           m_CalibrationFileName.c_str());
  }

  printf("[YDLIDAR INFO] Current %s AngleOffset : %f°\n",
         m_isAngleOffsetCorrected ? "corrected" : "uncorrrected", m_AngleOffset);
}

/*-------------------------------------------------------------
                        checkRobotOffsetAngleCorrected
-------------------------------------------------------------*/
void CYdLidar::checkRobotOffsetAngleCorrected(const string &serialNumber) {
  m_LRRAngleOffset = 0.0;
  m_isLRRAngleOffsetCorrected = false;

  if (ydlidar::fileExists(m_CalibrationFileName)) {
    SI_Error rc = ini.LoadFile(m_CalibrationFileName.c_str());

    if (rc >= 0) {
      m_isLRRAngleOffsetCorrected = true;
      double default_value = 90.6;
      m_LRRAngleOffset = ini.GetDoubleValue("ROBOT",
                                            serialNumber.c_str(),
                                            default_value);

      if (fabs(m_LRRAngleOffset - default_value) < 0.001) {
        m_LRRAngleOffset = 0.0;
        m_isLRRAngleOffsetCorrected = false;
      }

      printf("[YDLIDAR INFO] Successfully obtained the %s robot offset angle[%f] from the calibration file[%s]\n"
             , m_isLRRAngleOffsetCorrected ? "corrected" : "uncorrrected",
             m_LRRAngleOffset,
             m_CalibrationFileName.c_str());

    } else {
      printf("[YDLIDAR INFO] Failed to open calibration file[%s]\n",
             m_CalibrationFileName.c_str());
    }
  } else {
    printf("[YDLIDAR INFO] Calibration file[%s] does not exist\n",
           m_CalibrationFileName.c_str());
  }

  printf("[YDLIDAR INFO] Current %s RobotAngleOffset : %f°\n",
         m_isLRRAngleOffsetCorrected ? "corrected" : "uncorrrected",
         m_LRRAngleOffset);

}

/*-------------------------------------------------------------
                        saveRobotOffsetAngle
-------------------------------------------------------------*/
void CYdLidar::saveRobotOffsetAngle() {
  if (m_startRobotAngleOffset) {
    ini.SetDoubleValue("ROBOT", m_serial_number.c_str(),
                       m_LRRAngleOffset);
    SI_Error rc = ini.SaveFile(m_CalibrationFileName.c_str());

    if (rc >= 0) {
      m_startRobotAngleOffset = false;
      m_isLRRAngleOffsetCorrected = true;
      printf("[YDLIDAR INFO] Current robot offset correction value[%f] is saved\n",
             m_LRRAngleOffset);
    } else {
      fprintf(stderr, "Saving correction value[%f] failed\n",
              m_LRRAngleOffset);
      m_isLRRAngleOffsetCorrected = false;
    }
  }


}

void CYdLidar::lowSpeed() {
  if (!lidarPtr) {
    return;
  }

  if (!isScanning || !lidarPtr->isscanning()) {
    return ;
  }

  lidarPtr->setStartAdjSpeed();
  lidarPtr->setScanLowSpeed();

}

void CYdLidar::hightSpeed() {
  if (!lidarPtr) {
    return;
  }

  if (!isScanning || !lidarPtr->isscanning()) {
    return ;
  }

  lidarPtr->setStartAdjSpeed();
  lidarPtr->setScanHightSpeed();


}


/*-------------------------------------------------------------
            checkCOMMs
-------------------------------------------------------------*/
bool  CYdLidar::checkCOMMs() {
  if (!lidarPtr) {
    // create the driver instance
    lidarPtr = new YDlidarDriver();

    if (!lidarPtr) {
      fprintf(stderr, "Create Driver fail\n");
      return false;
    }
  }

  if (lidarPtr->isconnected()) {
    return true;
  }

  // Is it COMX, X>4? ->  "\\.\COMX"
  if (m_SerialPort.size() >= 3) {
    if (tolower(m_SerialPort[0]) == 'c' && tolower(m_SerialPort[1]) == 'o' &&
        tolower(m_SerialPort[2]) == 'm') {
      // Need to add "\\.\"?
      if (m_SerialPort.size() > 4 || m_SerialPort[3] > '4') {
        m_SerialPort = std::string("\\\\.\\") + m_SerialPort;
      }
    }
  }

  // make connection...
  result_t op_result = lidarPtr->connect(m_SerialPort.c_str(), m_SerialBaudrate);

  if (!IS_OK(op_result)) {
    fprintf(stderr,
            "[CYdLidar] Error, cannot bind to the specified serial port[%s] and baudrate[%d]\n",
            m_SerialPort.c_str(), m_SerialBaudrate);
    return false;
  }

  return true;
}

/*-------------------------------------------------------------
                        checkStatus
-------------------------------------------------------------*/
bool CYdLidar::checkStatus() {

  if (!checkCOMMs()) {
    return false;
  }

  bool ret = getDeviceHealth();

  if (!ret) {
    delay(1000);
  }

  if (!getDeviceInfo()) {
    delay(2000);
    ret = getDeviceInfo();

    if (!ret) {
      return false;
    }
  }

  lidarPtr->setIntensities(m_Intensities);
  lidarPtr->setIgnoreArray(m_IgnoreArray);
  return true;
}

/*-------------------------------------------------------------
                        checkHardware
-------------------------------------------------------------*/
bool CYdLidar::checkHardware() {
  if (!lidarPtr) {
    return false;
  }

  if (isScanning && lidarPtr->isscanning()) {
    return true;
  }

  return false;
}

/*-------------------------------------------------------------
            initialize
-------------------------------------------------------------*/
bool CYdLidar::initialize() {
  if (!checkCOMMs()) {
    fprintf(stderr,
            "[CYdLidar::initialize] Error initializing YDLIDAR check Comms.\n");
    fflush(stderr);
    return false;
  }

  if (!checkStatus()) {
    fprintf(stderr,
            "[CYdLidar::initialize] Error initializing YDLIDAR check status.\n");
    fflush(stderr);
    return false;
  }

  return true;
}
