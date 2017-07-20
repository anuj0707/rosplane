#include "estimator_base.h"
#include "estimator_example.h"

namespace rosplane {

estimator_base::estimator_base():
    nh_(ros::NodeHandle()),
    nh_private_(ros::NodeHandle("~"))
{
    nh_private_.param<std::string>("gps_topic", gps_topic_, "gps/data");
    nh_private_.param<std::string>("imu_topic", imu_topic_, "imu/data");
    nh_private_.param<std::string>("baro_topic", baro_topic_, "baro");
    nh_private_.param<std::string>("airspeed_topic", airspeed_topic_, "airspeed");
    nh_private_.param<std::string>("status_topic", status_topic_, "status");
    nh_private_.param<double>("update_rate", update_rate_, 100.0);
    params_.Ts = 1.0f/update_rate_;
    params_.gravity = 9.8;
    nh_private_.param<double>("rho", params_.rho, 1.225);
    nh_private_.param<double>("sigma_accel", params_.sigma_accel, 0.0245);
    nh_private_.param<double>("sigma_n_gps", params_.sigma_n_gps, 0.21);
    nh_private_.param<double>("sigma_e_gps", params_.sigma_e_gps, 0.21);
    nh_private_.param<double>("sigma_Vg_gps", params_.sigma_Vg_gps, 0.0500);
    nh_private_.param<double>("sigma_couse_gps", params_.sigma_course_gps, 0.0045);

    gps_sub_ = nh_.subscribe(gps_topic_, 10, &estimator_base::gpsCallback, this);
    imu_sub_ = nh_.subscribe(imu_topic_, 10, &estimator_base::imuCallback, this);
    baro_sub_ = nh_.subscribe(baro_topic_, 10, &estimator_base::baroAltCallback, this);
    airspeed_sub_ = nh_.subscribe(airspeed_topic_, 10, &estimator_base::airspeedCallback, this);
    status_sub_ = nh_.subscribe(status_topic_, 1, &estimator_base::statusCallback, this);
    update_timer_ = nh_.createTimer(ros::Duration(1.0/update_rate_), &estimator_base::update, this);
    vehicle_state_pub_ = nh_.advertise<rosplane_msgs::State>("state",10);
    _init_static = 0;
    _baro_count = 0;
    input_.armed_init = false;
}

void estimator_base::update(const ros::TimerEvent&)
{
    struct output_s output;
    if(input_.armed_init)
    {
        estimate(params_, input_, output);
    }
    input_.gps_new = false;

    rosplane_msgs::State msg;
    msg.header.stamp = ros::Time::now();
    msg.header.frame_id = 1; // Denotes global frame

    msg.position[0] = output.pn;
    msg.position[1] = output.pe;
    msg.position[2] = -output.h;
    if (gps_init_)
    {
      msg.initial_lat = init_lat_;
      msg.initial_lon = init_lon_;
      msg.initial_alt = init_alt_;
    }
    msg.Va = output.Va;
    msg.alpha = output.alpha;
    msg.beta = output.beta;
    msg.phi = output.phi;
    msg.theta = output.theta;
    msg.psi = output.psi;
    msg.chi = output.chi;
    msg.p = output.p;
    msg.q = output.q;
    msg.r = output.r;
    msg.Vg = output.Vg;
    msg.wn = output.wn;
    msg.we = output.we;
    msg.quat_valid = false;

    msg.psi_deg = fmod(output.psi,2*M_PI)*180/M_PI; //-360 to 360
    msg.psi_deg += (msg.psi_deg < -180 ? 360 : 0); msg.psi_deg -= (msg.psi_deg > 180 ? 360 : 0);
    msg.chi_deg = fmod(output.chi,2*M_PI)*180/M_PI; //-360 to 360
    msg.chi_deg += (msg.chi_deg < -180 ? 360 : 0); msg.chi_deg -= (msg.chi_deg > 180 ? 360 : 0);

    vehicle_state_pub_.publish(msg);
}

void estimator_base::gpsCallback(const rosflight_msgs::GPS &msg)
{
    if(msg.fix != true || msg.NumSat < 4 || !std::isfinite(msg.latitude))
    {
        input_.gps_new = false;
        return;
    }
    if(!gps_init_)
    {
        gps_init_ = true;
        init_alt_ = msg.altitude;
        init_lat_ = msg.latitude;
        init_lon_ = msg.longitude;
    }
    else
    {
        input_.gps_n = EARTH_RADIUS*(msg.latitude - init_lat_)*M_PI/180.0;
        input_.gps_e = EARTH_RADIUS*cos(init_lat_*M_PI/180.0)*(msg.longitude - init_lon_)*M_PI/180.0;
        input_.gps_h = msg.altitude - init_alt_;
        input_.gps_Vg = msg.speed;
        if(msg.speed > 0.3)
            input_.gps_course = msg.ground_course;
        input_.gps_new = true;
    }
}

void estimator_base::imuCallback(const sensor_msgs::Imu &msg)
{
    input_.accel_x = msg.linear_acceleration.x;
    input_.accel_y = msg.linear_acceleration.y;
    input_.accel_z = msg.linear_acceleration.z;

    input_.gyro_x = msg.angular_velocity.x;
    input_.gyro_y = msg.angular_velocity.y;
    input_.gyro_z = msg.angular_velocity.z;
}

void estimator_base::baroAltCallback(const rosflight_msgs::Barometer &msg)
{

    if(!_baro_init)
    {
        if(_baro_count < 100)
        {
            _init_static += msg.pressure;
            input_.static_pres = 0;
            _baro_count += 1;
        }
        else
        {
            _init_static = _init_static/100;
            _baro_init = true;
        }
    }
    else
    {
        float static_pres_old = input_.static_pres;
        input_.static_pres = -msg.pressure + _init_static;

        float gate_gain = 1.35*params_.rho*params_.gravity;
        if(input_.static_pres < static_pres_old - gate_gain)
        {
            input_.static_pres = static_pres_old - gate_gain;
        }
        else if(input_.static_pres > static_pres_old + gate_gain)
        {
            input_.static_pres = static_pres_old + gate_gain;
        }
    }
}

void estimator_base::airspeedCallback(const rosflight_msgs::Airspeed &msg)
{
    input_.diff_pres = msg.differential_pressure;
}

void estimator_base::statusCallback(const rosflight_msgs::Status &msg)
{
    input_.status_armed = msg.armed;
    if(input_.status_armed)
        input_.armed_init = true;
}

} //end namespace

int main(int argc, char** argv) {
  ros::init(argc, argv, "rosplane_estimator");
  rosplane::estimator_base* est = new rosplane::estimator_example();

  ros::spin();

  return 0;
}
