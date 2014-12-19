/*
 * TI Voxel Lib component.
 *
 * Copyright (c) 2014 Texas Instruments Inc.
 */

#include "DepthCamera.h"
#include "Logger.h"

namespace Voxel
{
  
bool DepthCamera::_addParameters(const Vector<ParameterPtr> &params)
{
  _parameters.reserve(_parameters.size() + params.size());
  
  for(const ParameterPtr &p: params)
  {
    if(_parameters.find(p->name()) == _parameters.end())
    {
      _parameters[p->name()] = p;
    }
    else
    {
      logger(ERROR) << "DepthCamera: Found an existing parameter in the list of parameters, with name " << p->name() << ". Not overwriting it." << endl;
      return false;
    }
  }
  return true;
}

bool DepthCamera::clearCallback()
{
  for(auto i = 0; i < CALLBACK_TYPE_COUNT; i++)
    _callback[i] = 0;
}

bool DepthCamera::registerCallback(FrameCallBackType type, CallbackType f)
{
  if(type < CALLBACK_TYPE_COUNT)
  {
    if(_callback[type])
      logger(WARNING) << "DepthCamera: " << id() << " already has a callback for this type = " << type << ". Overwriting it now." << std::endl;
    
    _callBackTypesRegistered |= (1 << type);
    _callback[type] = f;
    return true;
  }
  logger(ERROR) << "DepthCamera: Invalid callback type = " << type << " attempted for depth camera " << id() << std::endl;
  return false;
}

bool DepthCamera::_callbackAndContinue(uint32_t &callBackTypesToBeCalled, DepthCamera::FrameCallBackType type, const Frame &frame)
{
  if((callBackTypesToBeCalled | type) and _callback[type])
  {
    _callback[type](*this, frame, type);
  }
  
  callBackTypesToBeCalled &= ~type;
  
  return callBackTypesToBeCalled != 0;
}


void DepthCamera::_captureLoop()
{
  while(_running)
  {
    uint32_t callBackTypesToBeCalled = _callBackTypesRegistered;
    
    if(_callBackTypesRegistered == 0 or _callBackTypesRegistered == CALLBACK_RAW_FRAME_UNPROCESSED) // Only unprocessed frame types requested or none requested?
    {
      auto f = _rawFrameBuffers.get();
      if(_captureRawUnprocessedFrame(*f) and _callback)
        _callback[CALLBACK_RAW_FRAME_UNPROCESSED](*this, (Frame &)(**f), CALLBACK_RAW_FRAME_UNPROCESSED);
    }
    else
    {
      RawFramePtr f1;
      if(!_captureRawUnprocessedFrame(f1))
        continue;
      
      if(!_callbackAndContinue(callBackTypesToBeCalled, CALLBACK_RAW_FRAME_UNPROCESSED, *f1))
        continue;
      
      auto f = _rawFrameBuffers.get();
      
      if(!_processRawFrame(f1, *f))
        continue;
      
      if(!_callbackAndContinue(callBackTypesToBeCalled, CALLBACK_RAW_FRAME_PROCESSED, **f))
        continue;
      
      auto d = _depthFrameBuffers.get();
      
      if(!_convertToDepthFrame(*f, *d))
        continue;
      
      if(!_callbackAndContinue(callBackTypesToBeCalled, CALLBACK_DEPTH_FRAME, **d))
        continue;
      
      auto p = _pointCloudBuffers.get();
      
      if(!_convertToPointCloudFrame(*d, *p))
        continue;
      
      _callbackAndContinue(callBackTypesToBeCalled, CALLBACK_XYZI_POINT_CLOUD_FRAME, **p);
    }
  }
  
  if(!_running)
  {
    _stop();
  }
}

bool DepthCamera::_convertToPointCloudFrame(const DepthFramePtr &depthFrame, PointCloudFramePtr &pointCloudFrame)
{
  if(!depthFrame)
  {
    logger(ERROR) << "DepthCamera: Blank depth frame." << std::endl;
    return false;
  }
  
  XYZIPointCloudFrame *f = dynamic_cast<XYZIPointCloudFrame *>(pointCloudFrame.get());
  
  if(!f)
  {
    f = new XYZIPointCloudFrame();
    pointCloudFrame = PointCloudFramePtr(f);
  }
  
  f->id = depthFrame->id;
  f->timestamp = depthFrame->timestamp;
  f->points.resize(depthFrame->size.width*depthFrame->size.height);
  
  auto index = 0;
  
  auto x1 = 0, y1 = 0;
  
  auto theta = 0.0f, phi = 0.0f, thetaMax = 0.0f;
  
  auto w = depthFrame->size.width;
  auto h = depthFrame->size.height;
  
  auto scaleMax = sqrt(w*w/4.0f + h*h/4.0f);
  
  if(!getFieldOfView(thetaMax) or thetaMax == 0)
  {
    logger(ERROR) << "DepthCamera: Could not get the field of view angle for " << id() << std::endl;
    return false;
  }
  
  float focalLength;
  
  focalLength = scaleMax/tan(thetaMax);
  
  auto r = 0.0f;
  
  for(auto y = 0; y < h; y++)
    for(auto x = 0; x < w; x++, index++)
    {
      IntensityPoint &p = f->points[index];
      
      x1 = x - w/2;
      y1 = y - h/2;
      
      phi = atan(y1*1.0/x1);
      
      if(x1 < 0)
        phi = M_PI + phi; // atan() principal range [-PI/2, PI/2]. outside that add PI
      
      theta = atan(sqrt(x1*x1 + y1*y1)/focalLength);
      
      r = depthFrame->depth[index];
      p.i = depthFrame->amplitude[index];
      
      p.x = r*sin(theta)*cos(phi);
      p.y = r*sin(theta)*sin(phi);
      p.z = r*cos(theta);
      
      //logger(INFO) << "Point = " << p.i << "@(" << p.x << ", " << p.y << ", " << p.z << ")" << std::endl;
    }
    
  return true;
}


void DepthCamera::_captureThreadWrapper()
{
  _threadActive = true;
  _captureLoop();
  _threadActive = false;
}

bool DepthCamera::start()
{
  if(!_callback)
  {
    logger(ERROR) << "DepthCamera: Please register a callback to " << _id << " before starting capture" << std::endl;
    return false;
  }
  
  if(!_start())
    return false;
  
  _running = true;
  
  //_captureThreadWrapper();
  _captureThread = ThreadPtr(new Thread(&DepthCamera::_captureThreadWrapper, this));
  
  return true;
}

bool DepthCamera::stop()
{
  _running = false;
  return true;
}

void DepthCamera::wait()
{
  if(_threadActive)
    _captureThread->join();
}

DepthCamera::~DepthCamera()
{
  _rawFrameBuffers.clear();
  _depthFrameBuffers.clear();
  _pointCloudBuffers.clear();
  
  _parameters.clear();
}

bool DepthCamera::reset()
{
  if(!stop())
    return false;
  
  if(!_programmer->reset())
  {
    logger(ERROR) << "DepthCamera: Failed to reset device " << id() << std::endl;
    return false;
  }
  _programmer = nullptr;
  _streamer = nullptr;
  return true;
}


  
}