#pragma once

#include "../utility/utility.h"
#include "../parameters.h"

#include <ceres/ceres.h>
using namespace Eigen;

class IntegrationBase
{
  public:
    IntegrationBase() = delete;     //; 注意这种写法的意思，应该是不能使用默认构造函数？
    IntegrationBase(const Eigen::Vector3d &_acc_0, const Eigen::Vector3d &_gyr_0,
                    const Eigen::Vector3d &_linearized_ba, const Eigen::Vector3d &_linearized_bg)
        : acc_0{_acc_0}, gyr_0{_gyr_0}, linearized_acc{_acc_0}, linearized_gyr{_gyr_0},
          linearized_ba{_linearized_ba}, linearized_bg{_linearized_bg},
          jacobian{Eigen::Matrix<double, 15, 15>::Identity()}, covariance{Eigen::Matrix<double, 15, 15>::Zero()},
          sum_dt{0.0}, delta_p{Eigen::Vector3d::Zero()}, delta_q{Eigen::Quaterniond::Identity()}, delta_v{Eigen::Vector3d::Zero()}

    {
        //; 注意这里是18*18，因为使用中值积分
        noise = Eigen::Matrix<double, 18, 18>::Zero();
        //; 下面是设置噪声的协方差矩阵，各个变量之间是相互独立的，所以实际上只有对角线上有协方差块。
        //; ACC_N：加速度高斯白噪声的标准差    GYR_N：角速度的高斯白噪声的标准差
        //; ACC_W：加速度的零偏是随机游走（维纳过程），导数服从高斯分布，其标准差为ACC_W。   GYR_W同理
        //; 问题：这些标准差都是从配置文件中读出来的，那么这些值是从哪里来的呢？应该是数据集给出的，那么数据集是怎么给出的？标定它所使用的IMU得到的吗？
        noise.block<3, 3>(0, 0) =  (ACC_N * ACC_N) * Eigen::Matrix3d::Identity();
        noise.block<3, 3>(3, 3) =  (GYR_N * GYR_N) * Eigen::Matrix3d::Identity();
        noise.block<3, 3>(6, 6) =  (ACC_N * ACC_N) * Eigen::Matrix3d::Identity();
        noise.block<3, 3>(9, 9) =  (GYR_N * GYR_N) * Eigen::Matrix3d::Identity();
        noise.block<3, 3>(12, 12) =  (ACC_W * ACC_W) * Eigen::Matrix3d::Identity();
        noise.block<3, 3>(15, 15) =  (GYR_W * GYR_W) * Eigen::Matrix3d::Identity();
    }

    void push_back(double dt, const Eigen::Vector3d &acc, const Eigen::Vector3d &gyr)
    {
        // 相关时间差和传感器数据保留，方便后续repropagate
        dt_buf.push_back(dt);
        acc_buf.push_back(acc);
        gyr_buf.push_back(gyr);
        propagate(dt, acc, gyr);
    }

    /**
     * @brief 根据新设置的imu零偏重新对该帧进行预积分
     * 
     * @param[in] _linearized_ba 
     * @param[in] _linearized_bg 
     */
    void repropagate(const Eigen::Vector3d &_linearized_ba, const Eigen::Vector3d &_linearized_bg)
    {
        // 状态量全部清零
        sum_dt = 0.0;
        acc_0 = linearized_acc;
        gyr_0 = linearized_gyr;
        delta_p.setZero();
        delta_q.setIdentity();
        delta_v.setZero();
        // 赋上设置的零偏值
        linearized_ba = _linearized_ba;
        linearized_bg = _linearized_bg;
        jacobian.setIdentity();
        covariance.setZero();
        // 用之前存下来的imu值重新预积分
        for (int i = 0; i < static_cast<int>(dt_buf.size()); i++)
            propagate(dt_buf[i], acc_buf[i], gyr_buf[i]);
    }

    //; 中值积分实现IMU预积分的函数
    void midPointIntegration(double _dt, 
                            const Eigen::Vector3d &_acc_0, const Eigen::Vector3d &_gyr_0,
                            const Eigen::Vector3d &_acc_1, const Eigen::Vector3d &_gyr_1,
                            const Eigen::Vector3d &delta_p, const Eigen::Quaterniond &delta_q, const Eigen::Vector3d &delta_v,
                            const Eigen::Vector3d &linearized_ba, const Eigen::Vector3d &linearized_bg,
                            Eigen::Vector3d &result_delta_p, Eigen::Quaterniond &result_delta_q, Eigen::Vector3d &result_delta_v,
                            Eigen::Vector3d &result_linearized_ba, Eigen::Vector3d &result_linearized_bg, bool update_jacobian)
    {
        //ROS_INFO("midpoint integration");
        // 首先中值积分更新状态量
        Vector3d un_acc_0 = delta_q * (_acc_0 - linearized_ba);
        Vector3d un_gyr = 0.5 * (_gyr_0 + _gyr_1) - linearized_bg;
        result_delta_q = delta_q * Quaterniond(1, un_gyr(0) * _dt / 2, un_gyr(1) * _dt / 2, un_gyr(2) * _dt / 2);
        Vector3d un_acc_1 = result_delta_q * (_acc_1 - linearized_ba);
        Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
        //; 问题：这里的delta_v为什么不用平均值？解答：这里是公式的问题，不是中值积分的问题，匀加速运动变成变加速运动之后，
        //; 只有公式中的1/2*a*dt*dt变成a(t)的积分，其他都是不变的。
        result_delta_p = delta_p + delta_v * _dt + 0.5 * un_acc * _dt * _dt;
        result_delta_v = delta_v + un_acc * _dt;
        //; 在预积分的过程中，由于时间间隔非常小，可以认为零偏不发生改变
        result_linearized_ba = linearized_ba;
        result_linearized_bg = linearized_bg;         

        // 随后更新方差矩阵及雅克比
        if(update_jacobian)
        {
            Vector3d w_x = 0.5 * (_gyr_0 + _gyr_1) - linearized_bg;
            Vector3d a_0_x = _acc_0 - linearized_ba;
            Vector3d a_1_x = _acc_1 - linearized_ba;
            Matrix3d R_w_x, R_a_0_x, R_a_1_x;

            //; 以下三个是3d向量转反对称矩阵
            R_w_x<<0, -w_x(2), w_x(1),
                w_x(2), 0, -w_x(0),
                -w_x(1), w_x(0), 0;
            R_a_0_x<<0, -a_0_x(2), a_0_x(1),
                a_0_x(2), 0, -a_0_x(0),
                -a_0_x(1), a_0_x(0), 0;
            R_a_1_x<<0, -a_1_x(2), a_1_x(1),
                a_1_x(2), 0, -a_1_x(0),
                -a_1_x(1), a_1_x(0), 0;

            MatrixXd F = MatrixXd::Zero(15, 15);
            //; block访问格式：F.block <长，宽>（起始x, 起始y）
            F.block<3, 3>(0, 0) = Matrix3d::Identity();
            F.block<3, 3>(0, 3) = -0.25 * delta_q.toRotationMatrix() * R_a_0_x * _dt * _dt + 
                                  -0.25 * result_delta_q.toRotationMatrix() * R_a_1_x * (Matrix3d::Identity() - R_w_x * _dt) * _dt * _dt;
            F.block<3, 3>(0, 6) = MatrixXd::Identity(3,3) * _dt;
            F.block<3, 3>(0, 9) = -0.25 * (delta_q.toRotationMatrix() + result_delta_q.toRotationMatrix()) * _dt * _dt;
            F.block<3, 3>(0, 12) = -0.25 * result_delta_q.toRotationMatrix() * R_a_1_x * _dt * _dt * -_dt;
            F.block<3, 3>(3, 3) = Matrix3d::Identity() - R_w_x * _dt;
            F.block<3, 3>(3, 12) = -1.0 * MatrixXd::Identity(3,3) * _dt;
            F.block<3, 3>(6, 3) = -0.5 * delta_q.toRotationMatrix() * R_a_0_x * _dt + 
                                  -0.5 * result_delta_q.toRotationMatrix() * R_a_1_x * (Matrix3d::Identity() - R_w_x * _dt) * _dt;
            F.block<3, 3>(6, 6) = Matrix3d::Identity();
            F.block<3, 3>(6, 9) = -0.5 * (delta_q.toRotationMatrix() + result_delta_q.toRotationMatrix()) * _dt;
            F.block<3, 3>(6, 12) = -0.5 * result_delta_q.toRotationMatrix() * R_a_1_x * _dt * -_dt;
            F.block<3, 3>(9, 9) = Matrix3d::Identity();
            F.block<3, 3>(12, 12) = Matrix3d::Identity();
            //cout<<"A"<<endl<<A<<endl;

            MatrixXd V = MatrixXd::Zero(15,18);
            V.block<3, 3>(0, 0) =  0.25 * delta_q.toRotationMatrix() * _dt * _dt;
            V.block<3, 3>(0, 3) =  0.25 * -result_delta_q.toRotationMatrix() * R_a_1_x  * _dt * _dt * 0.5 * _dt;
            V.block<3, 3>(0, 6) =  0.25 * result_delta_q.toRotationMatrix() * _dt * _dt;
            V.block<3, 3>(0, 9) =  V.block<3, 3>(0, 3);
            V.block<3, 3>(3, 3) =  0.5 * MatrixXd::Identity(3,3) * _dt;
            V.block<3, 3>(3, 9) =  0.5 * MatrixXd::Identity(3,3) * _dt;
            V.block<3, 3>(6, 0) =  0.5 * delta_q.toRotationMatrix() * _dt;
            V.block<3, 3>(6, 3) =  0.5 * -result_delta_q.toRotationMatrix() * R_a_1_x  * _dt * 0.5 * _dt;
            V.block<3, 3>(6, 6) =  0.5 * result_delta_q.toRotationMatrix() * _dt;
            V.block<3, 3>(6, 9) =  V.block<3, 3>(6, 3);
            V.block<3, 3>(9, 12) = MatrixXd::Identity(3,3) * _dt;
            V.block<3, 3>(12, 15) = MatrixXd::Identity(3,3) * _dt;

            //step_jacobian = F;
            //step_V = V;
            //; 这里的雅克比是预积分量关于五个状态变量的导数，用于当偏差bias在后端优化后，使用一阶泰勒展开近似更新预积分量，避免重复计算预积分
            jacobian = F * jacobian;
            //; 下面的协方差是误差状态更新的协方差，用于后端优化的时候的信息矩阵，作为误差的权重来使用。其实上面推导F和V主要就是为了
            //; 更新下面的协方差矩阵。而上面同时使用F来更新预积分关于五个状态变量的雅克比只是一个副产品，并不是主要目的
            covariance = F * covariance * F.transpose() + V * noise * V.transpose();
        }
    }

    //; 传播函数
    void propagate(double _dt, const Eigen::Vector3d &_acc_1, const Eigen::Vector3d &_gyr_1)
    {
        dt = _dt;
        acc_1 = _acc_1;
        gyr_1 = _gyr_1;
        Vector3d result_delta_p;
        Quaterniond result_delta_q;
        Vector3d result_delta_v;
        Vector3d result_linearized_ba;
        Vector3d result_linearized_bg;

        //; 中值积分，真正实现预积分的函数，传入的参数如下：
                        //; 时间间隔，上帧加速度，上帧角速度，本帧加速度，本帧角速度，上帧预积分p,上帧预积分q,上帧预积分v
        midPointIntegration(_dt, acc_0, gyr_0, _acc_1, _gyr_1, delta_p, delta_q, delta_v,
                        //; 上帧加速度零偏，上帧角速度零偏
                            linearized_ba, linearized_bg,
                        //; 本次预积分p, 本帧预积分q, 本帧预积分v
                            result_delta_p, result_delta_q, result_delta_v,
                        //; 本帧加速度零偏，本帧角速度零偏， 是否计算雅克比的标志
                            result_linearized_ba, result_linearized_bg, 1);

        //checkJacobian(_dt, acc_0, gyr_0, acc_1, gyr_1, delta_p, delta_q, delta_v,
        //                    linearized_ba, linearized_bg);
        //; delta_p v q是预积分量，是预积分类的成员变量
        delta_p = result_delta_p;
        delta_q = result_delta_q;
        delta_v = result_delta_v;
        linearized_ba = result_linearized_ba;   //; 这里可见在预积分的过程中bias不发生改变
        linearized_bg = result_linearized_bg;
        delta_q.normalize();    //; 归一化，单位四元数才能表示旋转
        sum_dt += dt;

        acc_0 = acc_1;  //; 这里更新上一帧的imu数据
        gyr_0 = gyr_1;  
    }

    // 计算和给定相邻帧状态量的残差
    Eigen::Matrix<double, 15, 1> evaluate(const Eigen::Vector3d &Pi, const Eigen::Quaterniond &Qi, const Eigen::Vector3d &Vi, const Eigen::Vector3d &Bai, const Eigen::Vector3d &Bgi,
                                          const Eigen::Vector3d &Pj, const Eigen::Quaterniond &Qj, const Eigen::Vector3d &Vj, const Eigen::Vector3d &Baj, const Eigen::Vector3d &Bgj)
    {
        Eigen::Matrix<double, 15, 1> residuals;

        Eigen::Matrix3d dp_dba = jacobian.block<3, 3>(O_P, O_BA);
        Eigen::Matrix3d dp_dbg = jacobian.block<3, 3>(O_P, O_BG);

        Eigen::Matrix3d dq_dbg = jacobian.block<3, 3>(O_R, O_BG);

        Eigen::Matrix3d dv_dba = jacobian.block<3, 3>(O_V, O_BA);
        Eigen::Matrix3d dv_dbg = jacobian.block<3, 3>(O_V, O_BG);

        Eigen::Vector3d dba = Bai - linearized_ba;
        Eigen::Vector3d dbg = Bgi - linearized_bg;

        Eigen::Quaterniond corrected_delta_q = delta_q * Utility::deltaQ(dq_dbg * dbg);
        Eigen::Vector3d corrected_delta_v = delta_v + dv_dba * dba + dv_dbg * dbg;
        Eigen::Vector3d corrected_delta_p = delta_p + dp_dba * dba + dp_dbg * dbg;

        residuals.block<3, 1>(O_P, 0) = Qi.inverse() * (0.5 * G * sum_dt * sum_dt + Pj - Pi - Vi * sum_dt) - corrected_delta_p;
        residuals.block<3, 1>(O_R, 0) = 2 * (corrected_delta_q.inverse() * (Qi.inverse() * Qj)).vec();
        residuals.block<3, 1>(O_V, 0) = Qi.inverse() * (G * sum_dt + Vj - Vi) - corrected_delta_v;
        residuals.block<3, 1>(O_BA, 0) = Baj - Bai;
        residuals.block<3, 1>(O_BG, 0) = Bgj - Bgi;
        return residuals;
    }

    double dt;
    //; 因为使用中值积分，所以0和1分别存储这一帧和上一帧的加速度和角速度值
    Eigen::Vector3d acc_0, gyr_0;   
    Eigen::Vector3d acc_1, gyr_1;   

    //; 这个存储的是什么？为什么是const变量？
    const Eigen::Vector3d linearized_acc, linearized_gyr;
    Eigen::Vector3d linearized_ba, linearized_bg;

    //; 雅克比，协方差
    Eigen::Matrix<double, 15, 15> jacobian, covariance;
    Eigen::Matrix<double, 15, 15> step_jacobian;
    Eigen::Matrix<double, 15, 18> step_V;
    Eigen::Matrix<double, 18, 18> noise;

    double sum_dt;
    Eigen::Vector3d delta_p;
    Eigen::Quaterniond delta_q;
    Eigen::Vector3d delta_v;

    std::vector<double> dt_buf;
    std::vector<Eigen::Vector3d> acc_buf;
    std::vector<Eigen::Vector3d> gyr_buf;

};
/*

    void eulerIntegration(double _dt, const Eigen::Vector3d &_acc_0, const Eigen::Vector3d &_gyr_0,
                            const Eigen::Vector3d &_acc_1, const Eigen::Vector3d &_gyr_1,
                            const Eigen::Vector3d &delta_p, const Eigen::Quaterniond &delta_q, const Eigen::Vector3d &delta_v,
                            const Eigen::Vector3d &linearized_ba, const Eigen::Vector3d &linearized_bg,
                            Eigen::Vector3d &result_delta_p, Eigen::Quaterniond &result_delta_q, Eigen::Vector3d &result_delta_v,
                            Eigen::Vector3d &result_linearized_ba, Eigen::Vector3d &result_linearized_bg, bool update_jacobian)
    {
        result_delta_p = delta_p + delta_v * _dt + 0.5 * (delta_q * (_acc_1 - linearized_ba)) * _dt * _dt;
        result_delta_v = delta_v + delta_q * (_acc_1 - linearized_ba) * _dt;
        Vector3d omg = _gyr_1 - linearized_bg;
        omg = omg * _dt / 2;
        Quaterniond dR(1, omg(0), omg(1), omg(2));
        result_delta_q = (delta_q * dR);   
        result_linearized_ba = linearized_ba;
        result_linearized_bg = linearized_bg;         

        if(update_jacobian)
        {
            Vector3d w_x = _gyr_1 - linearized_bg;
            Vector3d a_x = _acc_1 - linearized_ba;
            Matrix3d R_w_x, R_a_x;

            R_w_x<<0, -w_x(2), w_x(1),
                w_x(2), 0, -w_x(0),
                -w_x(1), w_x(0), 0;
            R_a_x<<0, -a_x(2), a_x(1),
                a_x(2), 0, -a_x(0),
                -a_x(1), a_x(0), 0;

            MatrixXd A = MatrixXd::Zero(15, 15);
            // one step euler 0.5
            A.block<3, 3>(0, 3) = 0.5 * (-1 * delta_q.toRotationMatrix()) * R_a_x * _dt;
            A.block<3, 3>(0, 6) = MatrixXd::Identity(3,3);
            A.block<3, 3>(0, 9) = 0.5 * (-1 * delta_q.toRotationMatrix()) * _dt;
            A.block<3, 3>(3, 3) = -R_w_x;
            A.block<3, 3>(3, 12) = -1 * MatrixXd::Identity(3,3);
            A.block<3, 3>(6, 3) = (-1 * delta_q.toRotationMatrix()) * R_a_x;
            A.block<3, 3>(6, 9) = (-1 * delta_q.toRotationMatrix());
            //cout<<"A"<<endl<<A<<endl;

            MatrixXd U = MatrixXd::Zero(15,12);
            U.block<3, 3>(0, 0) =  0.5 * delta_q.toRotationMatrix() * _dt;
            U.block<3, 3>(3, 3) =  MatrixXd::Identity(3,3);
            U.block<3, 3>(6, 0) =  delta_q.toRotationMatrix();
            U.block<3, 3>(9, 6) = MatrixXd::Identity(3,3);
            U.block<3, 3>(12, 9) = MatrixXd::Identity(3,3);

            // put outside
            Eigen::Matrix<double, 12, 12> noise = Eigen::Matrix<double, 12, 12>::Zero();
            noise.block<3, 3>(0, 0) =  (ACC_N * ACC_N) * Eigen::Matrix3d::Identity();
            noise.block<3, 3>(3, 3) =  (GYR_N * GYR_N) * Eigen::Matrix3d::Identity();
            noise.block<3, 3>(6, 6) =  (ACC_W * ACC_W) * Eigen::Matrix3d::Identity();
            noise.block<3, 3>(9, 9) =  (GYR_W * GYR_W) * Eigen::Matrix3d::Identity();

            //write F directly
            MatrixXd F, V;
            F = (MatrixXd::Identity(15,15) + _dt * A);
            V = _dt * U;
            step_jacobian = F;
            step_V = V;
            jacobian = F * jacobian;
            covariance = F * covariance * F.transpose() + V * noise * V.transpose();
        }

    }     


    void checkJacobian(double _dt, const Eigen::Vector3d &_acc_0, const Eigen::Vector3d &_gyr_0, 
                                   const Eigen::Vector3d &_acc_1, const Eigen::Vector3d &_gyr_1,
                            const Eigen::Vector3d &delta_p, const Eigen::Quaterniond &delta_q, const Eigen::Vector3d &delta_v,
                            const Eigen::Vector3d &linearized_ba, const Eigen::Vector3d &linearized_bg)
    {
        Vector3d result_delta_p;
        Quaterniond result_delta_q;
        Vector3d result_delta_v;
        Vector3d result_linearized_ba;
        Vector3d result_linearized_bg;
        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            result_delta_p, result_delta_q, result_delta_v,
                            result_linearized_ba, result_linearized_bg, 0);

        Vector3d turb_delta_p;
        Quaterniond turb_delta_q;
        Vector3d turb_delta_v;
        Vector3d turb_linearized_ba;
        Vector3d turb_linearized_bg;

        Vector3d turb(0.0001, -0.003, 0.003);

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p + turb, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb p       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_jacobian.block<3, 3>(0, 0) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_jacobian.block<3, 3>(3, 0) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_jacobian.block<3, 3>(6, 0) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_jacobian.block<3, 3>(9, 0) * turb).transpose() << endl;
        cout << "bg diff " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff " << (step_jacobian.block<3, 3>(12, 0) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p, delta_q * Quaterniond(1, turb(0) / 2, turb(1) / 2, turb(2) / 2), delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb q       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_jacobian.block<3, 3>(0, 3) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_jacobian.block<3, 3>(3, 3) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_jacobian.block<3, 3>(6, 3) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_jacobian.block<3, 3>(9, 3) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_jacobian.block<3, 3>(12, 3) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p, delta_q, delta_v + turb,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb v       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_jacobian.block<3, 3>(0, 6) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_jacobian.block<3, 3>(3, 6) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_jacobian.block<3, 3>(6, 6) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_jacobian.block<3, 3>(9, 6) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_jacobian.block<3, 3>(12, 6) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba + turb, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb ba       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_jacobian.block<3, 3>(0, 9) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_jacobian.block<3, 3>(3, 9) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_jacobian.block<3, 3>(6, 9) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_jacobian.block<3, 3>(9, 9) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_jacobian.block<3, 3>(12, 9) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1, _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg + turb,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb bg       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_jacobian.block<3, 3>(0, 12) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_jacobian.block<3, 3>(3, 12) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_jacobian.block<3, 3>(6, 12) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_jacobian.block<3, 3>(9, 12) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_jacobian.block<3, 3>(12, 12) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0 + turb, _gyr_0, _acc_1 , _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb acc_0       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_V.block<3, 3>(0, 0) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_V.block<3, 3>(3, 0) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_V.block<3, 3>(6, 0) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_V.block<3, 3>(9, 0) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_V.block<3, 3>(12, 0) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0 + turb, _acc_1 , _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb _gyr_0       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_V.block<3, 3>(0, 3) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_V.block<3, 3>(3, 3) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_V.block<3, 3>(6, 3) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_V.block<3, 3>(9, 3) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_V.block<3, 3>(12, 3) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1 + turb, _gyr_1, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb acc_1       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_V.block<3, 3>(0, 6) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_V.block<3, 3>(3, 6) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_V.block<3, 3>(6, 6) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_V.block<3, 3>(9, 6) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_V.block<3, 3>(12, 6) * turb).transpose() << endl;

        midPointIntegration(_dt, _acc_0, _gyr_0, _acc_1 , _gyr_1 + turb, delta_p, delta_q, delta_v,
                            linearized_ba, linearized_bg,
                            turb_delta_p, turb_delta_q, turb_delta_v,
                            turb_linearized_ba, turb_linearized_bg, 0);
        cout << "turb _gyr_1       " << endl;
        cout << "p diff       " << (turb_delta_p - result_delta_p).transpose() << endl;
        cout << "p jacob diff " << (step_V.block<3, 3>(0, 9) * turb).transpose() << endl;
        cout << "q diff       " << ((result_delta_q.inverse() * turb_delta_q).vec() * 2).transpose() << endl;
        cout << "q jacob diff " << (step_V.block<3, 3>(3, 9) * turb).transpose() << endl;
        cout << "v diff       " << (turb_delta_v - result_delta_v).transpose() << endl;
        cout << "v jacob diff " << (step_V.block<3, 3>(6, 9) * turb).transpose() << endl;
        cout << "ba diff      " << (turb_linearized_ba - result_linearized_ba).transpose() << endl;
        cout << "ba jacob diff" << (step_V.block<3, 3>(9, 9) * turb).transpose() << endl;
        cout << "bg diff      " << (turb_linearized_bg - result_linearized_bg).transpose() << endl;
        cout << "bg jacob diff" << (step_V.block<3, 3>(12, 9) * turb).transpose() << endl;
    }
    */