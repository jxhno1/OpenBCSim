/*
Copyright (c) 2015, Sigurd Storve
All rights reserved.

Licensed under the BSD license.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <random> // for selecting scatterers

#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>
#include <QFileDialog>
#include <QDebug>
#include <QStatusBar>
#include <QSettings>
#include <QInputDialog>
#include <QTimer>
#include "ScopedCpuTimer.hpp"

#include "MainWindow.hpp"
#include "../HDFConvenience.hpp"
#include "../LibBCSim.hpp"
#include "utils.hpp" // needed for generating grayscale colortable
#include "SimpleHDF.hpp"    // for reading scatterer splines for vis.
#include "utils.hpp"        // for FirWin()
#include "ScanSequence.hpp"

#include "GLVisualizationWidget.hpp"
#include "scanseq/ScanseqWidget.hpp"
#include "BeamProfileWidget.hpp"
#include "ExcitationSignalWidget.hpp"
#include "SimulationParamsWidget.hpp"
#include "SignalProcessing.hpp"
#include "ScanGeometry.hpp"
#include "BCSimConfig.hpp"
#include "ProbeWidget.hpp"
#include "SimTimeWidget.hpp"
#include "GrayscaleTransformWidget.hpp"
#include "RefreshWorker.hpp"

MainWindow::MainWindow() {
    onLoadIniSettings();

    // Simulation time manager
    m_sim_time_manager = new SimTimeManager(0.0f, 1.0f);
    m_sim_time_manager->set_time(0.0f);
    m_sim_time_manager->set_time_delta(10e-3);

    // Create simulation time widget and configure signal connections.
    m_time_widget = new SimTimeWidget;
    connect(m_sim_time_manager, SIGNAL(min_time_changed(double)), m_time_widget, SLOT(set_min_time(double)));
    connect(m_sim_time_manager, SIGNAL(max_time_changed(double)), m_time_widget, SLOT(set_max_time(double)));
    connect(m_sim_time_manager, SIGNAL(time_changed(double)), m_time_widget, SLOT(set_time(double)));
    // Needed to update OpenGL visualization when time changes.
    connect(m_sim_time_manager, &SimTimeManager::time_changed, [&](double dummy) {
        updateOpenGlVisualization();
    });

    // Create main widget and its layout
    auto v_layout = new QVBoxLayout;
    auto h_layout = new QHBoxLayout;
    QWidget* window = new QWidget;
    window->setLayout(v_layout);
    setCentralWidget(window);


    m_gl_vis_widget = new GLVisualizationWidget;
    h_layout->addWidget(m_gl_vis_widget);

    // One column of all custom wiggets
    auto left_widget_col = new QVBoxLayout;
    auto right_widget_col = new QVBoxLayout;

    // Scansequence widget
    m_scanseq_widget = new ScanseqWidget;
    m_scanseq_widget->setMaximumWidth(200);
    left_widget_col->addWidget(m_scanseq_widget);
    
    // Probe widget
    m_probe_widget = new ProbeWidget;
    m_probe_widget->setMaximumWidth(200);
    left_widget_col->addWidget(m_probe_widget);
    
    // Beam profile widget
    m_beamprofile_widget = new GaussianBeamProfileWidget;
    m_beamprofile_widget->setMaximumWidth(200);
    connect(m_beamprofile_widget, SIGNAL(valueChanged(bcsim::IBeamProfile::s_ptr)), this, SLOT(onNewBeamProfile(bcsim::IBeamProfile::s_ptr)));
    left_widget_col->addWidget(m_beamprofile_widget);
    
    // Excitation signal widget
    m_excitation_signal_widget = new ExcitationSignalWidget;
    m_excitation_signal_widget->setMaximumWidth(200);
    right_widget_col->addWidget(m_excitation_signal_widget);
    connect(m_excitation_signal_widget, SIGNAL(valueChanged(bcsim::ExcitationSignal)), this, SLOT(onNewExcitation(bcsim::ExcitationSignal)));

    // General parameters widget
    m_simulation_params_widget = new SimulationParamsWidget;
    m_simulation_params_widget->setMaximumWidth(200);
    right_widget_col->addWidget(m_simulation_params_widget);

    // grayscale transform widget
    m_grayscale_widget = new GrayscaleTransformWidget;
    m_grayscale_widget->setMaximumWidth(200);
    right_widget_col->addWidget(m_grayscale_widget);

    h_layout->addLayout(left_widget_col);
    h_layout->addLayout(right_widget_col);

    v_layout->addLayout(h_layout);
    v_layout->addWidget(m_time_widget);

    createMenus();
    const auto scatterers_file = m_settings->value("default_scatterers").toString();
    loadScatterers(scatterers_file.toUtf8().constData());

    m_label = new QLabel("No simulation data");
    h_layout->addWidget(m_label);

    // Playback timer
    m_playback_timer = new QTimer;
    m_playback_millisec = 1;
    connect(m_playback_timer, SIGNAL(timeout()), this, SLOT(onTimer()));

    int num_lines;
    auto geometry = m_scanseq_widget->get_geometry(num_lines);
    newScansequence(geometry, num_lines);
    m_save_images = false;

    // refresh thread setup
    qRegisterMetaType<refresh_worker::WorkTask::ptr>();
    qRegisterMetaType<refresh_worker::WorkResult::ptr>();
    m_refresh_worker = new refresh_worker::RefreshWorker(33);
    connect(m_refresh_worker, &refresh_worker::RefreshWorker::processed_data_available, [&](refresh_worker::WorkResult::ptr work_result) {
        work_result->image.setColorTable(GrayColortable());
        
        m_label->setPixmap(QPixmap::fromImage(work_result->image));
        if (m_save_images) {
            // TODO: Have an object that remebers path and can save the geometry file (parameters.txt)
            const auto img_path = m_settings->value("png_output_folder", "d:/temp").toString();
            const QString filename =  img_path + QString("/frame%1.png").arg(m_num_simulated_frames, 6, 10, QChar('0'));
            qDebug() << "Simulation time is " << m_sim_time_manager->get_time() << ". Writing image to" << filename;
            work_result->image.save(filename);
        }
        // store updated normalization constant if enabled.
        auto temp = m_grayscale_widget->get_values();
        if (temp.auto_normalize) {
            m_grayscale_widget->set_normalization_constant(work_result->updated_normalization_const);
        }
    });
}

void MainWindow::onLoadIniSettings() {
    // TODO: Use smart pointer to avoid memleak.
    const QString ini_file("settings.ini");
    QFileInfo ini_file_info(ini_file);
    if (ini_file_info.exists()) {
        qDebug() << "Found " << ini_file << ". Using settings from this file";
    } else {
        qDebug() << "Unable to find " << ini_file << ". Using default settings.";
    }
    m_settings = new QSettings("settings.ini", QSettings::IniFormat);
}


void MainWindow::createMenus() {
    auto menuBar = new QMenuBar;
    auto fileMenu =     menuBar->addMenu(tr("&File"));
    auto simulateMenu = menuBar->addMenu(tr("&Simulate"));
    auto about_menu = menuBar->addMenu(tr("&About"));
    
    // Create all actions in "File" menu
    auto loadScatterersAct = new QAction(tr("Load scatterers [fixed or spline]"), this);
    connect(loadScatterersAct, SIGNAL(triggered()), this, SLOT(onLoadScatterers()));
    fileMenu->addAction(loadScatterersAct);

    auto loadExcitationAct = new QAction(tr("Load excitation signal"), this);
    connect(loadExcitationAct, SIGNAL(triggered()), this, SLOT(onLoadExcitation()));
    fileMenu->addAction(loadExcitationAct);

    auto gpu_algorithm_act = new QAction(tr("Create a GPU simulator"), this);
    connect(gpu_algorithm_act, SIGNAL(triggered()), this, SLOT(onCreateGpuSimulator()));
    fileMenu->addAction(gpu_algorithm_act);

    auto gpu_load_scatterers_act = new QAction(tr("Load new scatterers for GPU"), this);
    connect(gpu_load_scatterers_act, SIGNAL(triggered()), this, SLOT(onGpuLoadScatterers()));
    fileMenu->addAction(gpu_load_scatterers_act);
    
    auto refresh_settings_act = new QAction(tr("Refresh settings"), this);
    connect(refresh_settings_act, SIGNAL(triggered()), this, SLOT(onLoadIniSettings()));
    fileMenu->addAction(refresh_settings_act);

    auto exitAct = new QAction(tr("Exit"), this);
    connect(exitAct, SIGNAL(triggered()), this, SLOT(onExit()));
    fileMenu->addAction(exitAct);
    
    // Create all actions in "Simulate" menu
    auto simulateAct = new QAction(tr("Simulate"), this);
    connect(simulateAct, SIGNAL(triggered()), this, SLOT(onSimulate()));
    simulateMenu->addAction(simulateAct);

    auto toggle_save_image_act = new QAction(tr("Save images"), this);
    toggle_save_image_act->setCheckable(true);
    toggle_save_image_act->setChecked(m_save_images);
    connect(toggle_save_image_act, SIGNAL(triggered(bool)), this, SLOT(onToggleSaveImage(bool)));
    simulateMenu->addAction(toggle_save_image_act);

    auto save_cartesian_limits_act = new QAction(tr("Save xy extent"), this);
    connect(save_cartesian_limits_act, &QAction::triggered, [&]() {
        const auto img_path = m_settings->value("png_output_folder", "d:/temp").toString();
        const auto out_file = img_path + "/parameters.ini";
        QFile f(out_file);
        if (f.open(QIODevice::WriteOnly)) {
            float x_min, x_max, y_min, y_max;
            m_scan_geometry->get_xy_extent(x_min, x_max, y_min, y_max);
            QTextStream stream(&f);
            stream << "width_meters = " << (x_max-x_min) << "\n";
            stream << "height_meters = " << (y_max-y_min) << "\n";
        } else {
            throw std::runtime_error("failed to open file for writing");
        }
    });
    simulateMenu->addAction(save_cartesian_limits_act);

    // TODO: Finish implementation
    auto setTimeAct = new QAction(tr("Set time"), this);
    connect(setTimeAct, SIGNAL(triggered()), this, SLOT(onSetSimTme()));
    simulateMenu->addAction(setTimeAct);

    auto set_noise_act = new QAction(tr("Set noise amplitude"), this);
    connect(set_noise_act, &QAction::triggered, this, &MainWindow::onSetSimulatorNoise);
    simulateMenu->addAction(set_noise_act);

    auto start_timer_act = new QAction(tr("Start timer"), this);
    connect(start_timer_act, &QAction::triggered, this, &MainWindow::onStartTimer);
    simulateMenu->addAction(start_timer_act);

    auto stop_timer_act = new QAction(tr("Stop timer"), this);
    connect(stop_timer_act, &QAction::triggered, this, &MainWindow::onStopTimer);
    simulateMenu->addAction(stop_timer_act);

    auto playback_speed_act = new QAction(tr("Set playback speed"), this);
    connect(playback_speed_act, &QAction::triggered, this, &MainWindow::onSetPlaybackSpeed);
    simulateMenu->addAction(playback_speed_act);

    // Actions in about menu
    auto about_scatterers_act = new QAction(tr("Scatterers details"), this);
    connect(about_scatterers_act, &QAction::triggered, this, &MainWindow::onAboutScatterers);
    about_menu->addAction(about_scatterers_act);

    auto get_xy_extent_act = new QAction(tr("Get Cartesian scan limits"), this);
    connect(get_xy_extent_act, &QAction::triggered, this, &MainWindow::onGetXyExtent);
    about_menu->addAction(get_xy_extent_act);

    setMenuBar(menuBar);

}

void MainWindow::onLoadScatterers() {
    auto h5_file = QFileDialog::getOpenFileName(this, tr("Load h5 scatterer dataset"), "", tr("h5 files (*.h5)"));
    if (h5_file == "") {
        qDebug() << "Invalid scatterer file. Skipping";
        return;
    }
    loadScatterers(h5_file);
}

void MainWindow::onLoadExcitation() {
    auto h5_file = QFileDialog::getOpenFileName(this, tr("Load h5 excitation signal"), "", tr("h5 files (*.h5)"));
    if (h5_file == "") {
        qDebug() << "Invalid excitation file. Skipping";
        return;
    }
    setExcitation(h5_file);
}

void MainWindow::onCreateGpuSimulator() {
    QStringList items;
    items << "gpu_fixed" << "gpu_spline";
    bool ok;
    QString item = QInputDialog::getItem(this, tr("Select GPU algorithm type"),
                                         tr("Type:"), items, 0, false, &ok);
    if (ok && !item.isEmpty()) {
        try {
            m_sim = bcsim::IAlgorithm::s_ptr(bcsim::Create(item.toUtf8().constData()));
        } catch (const std::runtime_error& e) {
            qDebug() << "Caught exception: " << e.what();
            onExit();
        }
        // This must currently be done before defining the scanseq.
        onGpuLoadScatterers();

        // GPU-specific hack.
        bcsim::SimulationParams params;
        params.sound_speed = 1540.0f;
        m_sim->set_parameters(params);

        // configure excitation
        m_sim->set_excitation(m_current_excitation);
        
        // configure scanseq and excitation
        int num_lines;
        auto scan_geometry = m_scanseq_widget->get_geometry(num_lines);
        newScansequence(scan_geometry, num_lines);
        
        // configure Gaussian beam profile
        const auto sigma_lateral     = m_beamprofile_widget->get_lateral_sigma();
        const auto sigma_elevational = m_beamprofile_widget->get_elevational_sigma();
        m_sim->set_beam_profile(bcsim::IBeamProfile::s_ptr(new bcsim::GaussianBeamProfile(sigma_lateral, sigma_elevational)));

        updateOpenGlVisualization();
    }
}

void MainWindow::onGpuLoadScatterers() {
    auto h5_file = QFileDialog::getOpenFileName(this, tr("Load h5 scatterer dataset"), "", tr("h5 files (*.h5)"));
    if (h5_file == "") {
        qDebug() << "Invalid scatterer file. Skipping";
        return;
    }
    auto temp = std::string(h5_file.toUtf8().constData());
    auto scatterers_type = bcsim::AutodetectScatteresType(temp);
    if (scatterers_type == "fixed") {
        bcsim::setFixedScatterersFromHdf(m_sim, temp);

        // Handle visualization in OpenGL
        initializeFixedVisualization(h5_file);

    } else if (scatterers_type == "spline") {
        bcsim::setSplineScatterersFromHdf(m_sim, temp);

        // Handle visualization in OpenGL
        initializeSplineVisualization(h5_file);
    } else {
        throw std::runtime_error("Invalid autodetected scatterer type");
    }
}

void MainWindow::onSimulate() {
    doSimulation();
}

void MainWindow::onSetSimulatorNoise() {
    bool ok;
    auto noise_amplitude = QInputDialog::getDouble(this, tr("New simulator noise value"), tr("New amplitude:"), 0.0, 0.0, 10e6, 3, &ok);
    if (!ok) {
        return;
    }

    qDebug() << "Setting new noise amplitude: " << noise_amplitude;
    m_sim->set_noise_amplitude(static_cast<float>(noise_amplitude));
}

void MainWindow::loadScatterers(const QString h5_file) {
    const std::string h5_file_str = h5_file.toUtf8().constData();
    auto type = bcsim::AutodetectScatteresType(h5_file_str);
    
    if (type == "fixed") {
        initializeSimulator("fixed");
        try {
            m_current_scatterers = bcsim::loadFixedScatterersFromHdf(h5_file_str);
            m_sim->set_scatterers(m_current_scatterers);
        } catch(const std::runtime_error& e) {
            qDebug() << "Caught exception: " << e.what();
        }

        // Handle visualization in OpenGL
        initializeFixedVisualization(h5_file);
    } else if (type == "spline") {
        initializeSimulator("spline");
        try {
            m_current_scatterers = bcsim::loadSplineScatterersFromHdf(h5_file_str);
            m_sim->set_scatterers(m_current_scatterers);

            auto temp = std::dynamic_pointer_cast<bcsim::SplineScatterers>(m_current_scatterers);
            
            // update simulation time limits
            const auto min_time = temp->knot_vector.front();
            const auto max_time = temp->knot_vector.back() - 1e-6f; // "end-hack"
            m_sim_time_manager->set_min_time(min_time);
            m_sim_time_manager->set_max_time(max_time);
            m_sim_time_manager->reset();
            qDebug() << "Spline scatterers time interval is [" << min_time << ", " << max_time << "]";

        } catch(const std::runtime_error& e) {
            qDebug() << "Caught exception: " << e.what();
        }

        // Handle visualization in OpenGL
        initializeSplineVisualization(h5_file);
    }
    qDebug() << "Configured scatterers";

    updateOpenGlVisualization();
}

void MainWindow::newScansequence(bcsim::ScanGeometry::ptr new_geometry, int new_num_lines) {
    const auto cur_time = m_sim_time_manager->get_time();

    // Get probe origin and orientation corresponding to current simulation time.
    auto temp_probe_origin = m_probe_widget->get_origin(cur_time);
    bcsim::vector3 probe_origin(temp_probe_origin.x(), temp_probe_origin.y(), temp_probe_origin.z());
    
    auto temp_rot_angles = m_probe_widget->get_rot_angles(cur_time);
    bcsim::vector3 rot_angles(temp_rot_angles.x(), temp_rot_angles.y(), temp_rot_angles.z());

    m_scan_geometry = new_geometry;
    //qDebug() << "Probe orientation: " << rot_angles.x << rot_angles.y << rot_angles.z;
    auto new_scanseq = bcsim::OrientScanSequence(bcsim::CreateScanSequence(new_geometry, new_num_lines, cur_time), rot_angles, probe_origin);

    m_sim->set_scan_sequence(new_scanseq);
    m_gl_vis_widget->setScanSequence(new_scanseq);
    updateOpenGlVisualization();
}

void MainWindow::setExcitation(const QString h5_file) {
    throw std::runtime_error("this function should not be used");
    try {
        auto new_excitation = bcsim::loadExcitationFromHdf(h5_file.toUtf8().constData());
        m_current_excitation = new_excitation;
        m_sim->set_excitation(new_excitation);
        qDebug() << "Configured excitation";
    } catch (const std::runtime_error& e) {
        qDebug() << "Caught exception: " << e.what();
    }
}

void MainWindow::initializeSimulator(const std::string& type) {
    try {
        m_sim = bcsim::IAlgorithm::s_ptr(bcsim::Create(type));
        m_num_simulated_frames = 0;
    } catch (const std::runtime_error& e) {
        qDebug() << "Caught exception: " << e.what();
        onExit();
    }

    m_sim->set_verbose(false);
    const int num_cores = m_settings->value("cpu_sim_num_cores", 1).toInt();
    m_sim->set_use_specific_num_cores(num_cores);
    bcsim::SimulationParams params;
    params.sound_speed = 1540.0f;
    m_sim->set_parameters(params);

    // For now hardcoded to use analytic Gaussian beam profile
    //auto beam_profile = m_beamprofile_widget->getValue();
    auto beam_profile = bcsim::IBeamProfile::s_ptr(new bcsim::GaussianBeamProfile(0.5e-3f, 1.0e-3f));
    m_sim->set_beam_profile(beam_profile);

    // Configure simulator to do envelope detection
    m_sim->set_output_type("env");

    qDebug() << "Created simulator";
    // force-emit from all widgets to ensure a fully configured simulator.
    m_excitation_signal_widget->force_emit();
}

void MainWindow::doSimulation() {
    // recreate scanseq to ensure correct time and probe info in case of dynamic probe.
    int new_num_scanlines;
    auto new_scan_geometry = m_scanseq_widget->get_geometry(new_num_scanlines);
    newScansequence(new_scan_geometry, new_num_scanlines);
    
    //qDebug() << "doSimulation(): simulation time is " << m_sim_time_manager->get_time();

    std::vector<std::vector<bc_float> > rf_lines;
    int simulation_millisec;

    try {
        ScopedCpuTimer timer([&](int millisec) {
            simulation_millisec = millisec;
        });
        m_sim->simulate_lines(rf_lines);
        m_num_simulated_frames++;
    
        // Create refresh work task from current geometry and the beam space data
        auto refresh_task = refresh_worker::WorkTask::ptr(new refresh_worker::WorkTask);
        refresh_task->set_geometry(m_scan_geometry);
        refresh_task->set_data(rf_lines);
        auto grayscale_settings = m_grayscale_widget->get_values();
        refresh_task->set_normalize_const(grayscale_settings.normalization_const);
        refresh_task->set_auto_normalize(grayscale_settings.auto_normalize);
        refresh_task->set_dots_per_meter( m_settings->value("qimage_dots_per_meter", 6000.0f).toFloat() );
        refresh_task->set_dyn_range(grayscale_settings.dyn_range);
        refresh_task->set_gain(grayscale_settings.gain); 
    
        m_refresh_worker->process_data(refresh_task);
    
    } catch (const std::runtime_error& e) {
        qDebug() << "Caught exception: " << e.what();
    }
    statusBar()->showMessage("Simulation time: " + QString::number(simulation_millisec) + " ms.");
}

// Currently ignoring weights when visualizing
void MainWindow::initializeFixedVisualization(const QString& h5_file) {
    SimpleHDF::SimpleHDF5Reader reader(h5_file.toUtf8().constData());
    auto data =  reader.readMultiArray<float, 2>("data");
    auto shape = data.shape();
    auto dimensionality = data.num_dimensions();
    Q_ASSERT(dimensionality == 2);
    auto num_scatterers = shape[0];
    auto num_comp = shape[2];
    Q_ASSERT(num_comp == 4);

    qDebug() << "Number of scatterers is " << num_scatterers;
    int num_vis_scatterers = m_settings->value("num_opengl_scatterers", 1000).toInt();
    qDebug() << "Number of visualization scatterers is " << num_vis_scatterers;

    // Select random indices into scatterers
    std::random_device rd;
    std::mt19937 eng(rd());
    std::uniform_int_distribution<> distr(0, static_cast<int>(num_scatterers)-1);

    std::vector<bcsim::vector3> scatterer_points(num_vis_scatterers);
    for (int scatterer_no = 0; scatterer_no < num_vis_scatterers; scatterer_no++) {
        int ind = distr(eng);
        const auto p = bcsim::vector3(data[ind][0], data[ind][1], data[ind][2]);
        scatterer_points[scatterer_no] = p;
    }
    m_gl_vis_widget->setFixedScatterers(scatterer_points);
}


// Currently ignoring weights when visualizing
void MainWindow::initializeSplineVisualization(const QString& h5_file) {
    SimpleHDF::SimpleHDF5Reader reader(h5_file.toUtf8().constData());
    auto nodes       =  reader.readMultiArray<float, 3>("nodes");
    auto knot_vector =  reader.readStdVector<float>("knot_vector");
    int spline_degree = reader.readScalar<int>("spline_degree");

    auto shape = nodes.shape();
    auto dimensionality = nodes.num_dimensions();
    Q_ASSERT(dimensionality == 3);
    int num_scatterers = static_cast<int>(shape[0]);
    int num_cs = static_cast<int>(shape[1]);
    int num_comp = static_cast<int>(shape[2]);
    Q_ASSERT(num_comp == 4);
    qDebug() << "Number of scatterers is " << num_scatterers;
    qDebug() << "Each scatterer has " << num_cs << " control points";
    int num_vis_scatterers = m_settings->value("num_opengl_scatterers", 1000).toInt();
    qDebug() << "Number of visualization scatterers is " << num_vis_scatterers;

    num_vis_scatterers = std::min(num_vis_scatterers, num_scatterers);

    // Select random indices into scatterers
    std::random_device rd;
    std::mt19937 eng(rd());
    std::uniform_int_distribution<> distr(0, num_scatterers-1);

    std::vector<SplineCurve<float, bcsim::vector3> > splines(num_vis_scatterers);
    for (int scatterer_no = 0; scatterer_no < num_vis_scatterers; scatterer_no++) {
        int ind = distr(eng);
        
        // Create a SplineCurve for current scatterer
        SplineCurve<float, bcsim::vector3> curve;
        curve.knots = knot_vector;
        curve.degree = spline_degree;
        curve.cs.resize(num_cs);
        for (int cs_no = 0; cs_no < num_cs; cs_no++) {
            curve.cs[cs_no] = bcsim::vector3(nodes[ind][cs_no][0], nodes[ind][cs_no][1], nodes[ind][cs_no][2]);
        }
        splines[scatterer_no] = curve;
    }

    // Pass new splines the visualization widget
    m_gl_vis_widget->setScattererSplines(splines);
}

void MainWindow::onNewExcitation(bcsim::ExcitationSignal new_excitation) {
    m_current_excitation = new_excitation;
    m_sim->set_excitation(new_excitation);
    qDebug() << "Configured excitation signal";
}

void MainWindow::onNewBeamProfile(bcsim::IBeamProfile::s_ptr new_beamprofile) {
    m_sim->set_beam_profile(new_beamprofile);
    qDebug() << "Configured beam profile";
}

void MainWindow::onStartTimer() {
    m_playback_timer->start(m_playback_millisec);
}

void MainWindow::onStopTimer() {
    m_playback_timer->stop();
}

void MainWindow::onSetPlaybackSpeed() {
    bool ok;
    auto dt = QInputDialog::getDouble(this, "Simulation dt", "Time [s]", 1e-3, 0.0, 100.0, 5, &ok);
    if (ok) {
        m_sim_time_manager->set_time_delta(dt);
    }
}

void MainWindow::onSetSimTme() {
    bool ok;
    auto sim_time = QInputDialog::getDouble(this, "Simulation time", "Time [s]",
                                      m_sim_time_manager->get_time(),
                                      m_sim_time_manager->get_min_time(),
                                      m_sim_time_manager->get_max_time(), 5, &ok);
    if (ok) {
        m_sim_time_manager->set_time(sim_time);
    }
}

void MainWindow::onTimer() {
    m_sim_time_manager->advance();
    ScopedCpuTimer timer([](int millisec) {
        std::cout << "onTimer() used: " << millisec << " ms.\n";
    });
    onSimulate();
}

void MainWindow::onAboutScatterers() {
    QString info = "Phantom consists of " + QString::number(m_current_scatterers->num_scatterers());
    auto spline_scatterers = std::dynamic_pointer_cast<bcsim::SplineScatterers>(m_current_scatterers);
    auto fixed_scatterers = std::dynamic_pointer_cast<bcsim::FixedScatterers>(m_current_scatterers);
    
    if (spline_scatterers) {
        info += " spline scatterers of degree " + QString::number(spline_scatterers->spline_degree);
        info += ", each consisting of " + QString::number(spline_scatterers->nodes[0].size()) + " control points.";
    } else if (fixed_scatterers) {
        info += " fixed scatterers.";
    } else {
        throw std::runtime_error("onAboutScatterers(): all casts failed");
    }
    QMessageBox::information(this, "Current scatterers", info); 
}

void MainWindow::onGetXyExtent() {
    float x_min, x_max, y_min, y_max;
    m_scan_geometry->get_xy_extent(x_min, x_max, y_min, y_max);
    auto info = QString("x=%1...%2, y=%3...%4").arg(QString::number(x_min), QString::number(x_max), QString::number(y_min), QString::number(y_max));
    info += QString("\nWidth is %1. Height is %2").arg(QString::number(x_max-x_min), QString::number(y_max-y_min));
    QMessageBox::information(this, "Cartesian scan limits", info);
}

void MainWindow::updateOpenGlVisualization() {
    if (!m_gl_vis_widget) return;

    // Update scatterer visualization
    auto new_timestamp = m_sim_time_manager->get_time();
    m_gl_vis_widget->updateTimestamp(new_timestamp);
}