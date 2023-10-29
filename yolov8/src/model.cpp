#include <math.h>
#include <iostream>

#include "model.h"
#include "block.h"
#include "calibrator.h"
#include "config.h"

using namespace nvinfer1;
static int get_width(int x, float gw, int max_channels, int divisor = 8) {
    auto channel = int(ceil((x * gw) / divisor)) * divisor;
    return channel >= max_channels ? max_channels : channel;
}

static int get_depth(int x, float gd) {
    if (x == 1) return 1;
    int r = round(x * gd);
    if (x * gd - int(x * gd) == 0.5 && (int(x * gd) % 2) == 0) --r;
    return std::max<int>(r, 1);
}

static IElementWiseLayer* Proto(INetworkDefinition* network, std::map<std::string, Weights>& weightMap,
                                          ITensor& input, std::string lname, float gw, int max_channels) {
    int mid_channel = get_width(256, gw, max_channels);
    auto cv1 = convBnSiLU(network, weightMap, input, mid_channel, 3, 1, 1, "model.22.proto.cv1");
    float* convTranpsose_bais = (float*)weightMap["model.22.proto.upsample.bias"].values;
    int convTranpsose_bais_len = weightMap["model.22.proto.upsample.bias"].count;
    Weights bias{DataType::kFLOAT, convTranpsose_bais, convTranpsose_bais_len};
    auto convTranpsose  = network->addDeconvolutionNd(*cv1->getOutput(0), mid_channel, DimsHW{2,2}, weightMap["model.22.proto.upsample.weight"], bias);
    assert(convTranpsose);
    convTranpsose->setStrideNd(DimsHW{2, 2});
    auto cv2 = convBnSiLU(network,weightMap,*convTranpsose->getOutput(0), mid_channel, 3, 1, 1, "model.22.proto.cv2");
    auto cv3 = convBnSiLU(network,weightMap,*cv2->getOutput(0), 32, 1, 1, 0,"model.22.proto.cv3");
    assert(cv3);
    return cv3;
}

static IShuffleLayer* ProtoCoef(INetworkDefinition* network, std::map<std::string, Weights>& weightMap,
                                          ITensor& input, std::string lname, int grid_shape, float gw) {

    int mid_channle = 0;
    if(gw == 0.25 || gw== 0.5) {
        mid_channle = 32;
    } else if(gw == 0.75) {
        mid_channle = 48;
    } else if(gw == 1.00) {
        mid_channle = 64;
    } else if(gw == 1.25) {
        mid_channle = 80;
    }
    auto cv0 = convBnSiLU(network, weightMap, input, mid_channle, 3, 1, 1, lname + ".0");
    auto cv1 = convBnSiLU(network, weightMap, *cv0->getOutput(0), mid_channle, 3, 1, 1, lname + ".1");
    float* cv2_bais_value = (float*)weightMap[lname + ".2" + ".bias"].values;
    int cv2_bais_len = weightMap[lname + ".2" + ".bias"].count;
    Weights cv2_bais{DataType::kFLOAT, cv2_bais_value, cv2_bais_len};
    auto cv2 = network->addConvolutionNd(*cv1->getOutput(0), 32, DimsHW{1, 1}, weightMap[lname + ".2" + ".weight"], cv2_bais);
    cv2->setStrideNd(DimsHW{1, 1});
    IShuffleLayer* cv2_shuffle = network->addShuffle(*cv2->getOutput(0));
    cv2_shuffle->setReshapeDimensions(Dims2{ 32, grid_shape});
    return cv2_shuffle;
}

IHostMemory* buildEngineYolov8Det(IBuilder* builder,
                                            IBuilderConfig* config, DataType dt,
                                            const std::string& wts_path, float& gd, float& gw, int& max_channels) {
    std::map<std::string, Weights> weightMap = loadWeights(wts_path);
    INetworkDefinition* network = builder->createNetworkV2(0U);

    /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
    ITensor* data = network->addInput(kInputTensorName, dt, Dims3{3, kInputH, kInputW});
    assert(data);

    /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
    IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
    IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
    // 11233
    IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
    IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
    // 22466
    IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
    IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
    // 22466
    IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
    IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
    // 11233
    IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");
    IElementWiseLayer* conv9 = SPPF(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.9");

    /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
    float scale[] = {1.0, 2.0, 2.0};
    IResizeLayer* upsample10 = network->addResize(*conv9->getOutput(0));
    assert(upsample10);
    upsample10->setResizeMode(ResizeMode::kNEAREST);
    upsample10->setScales(scale, 3);

    ITensor* inputTensor11[] = {upsample10->getOutput(0), conv6->getOutput(0)};
    IConcatenationLayer* cat11 = network->addConcatenation(inputTensor11, 2);
    IElementWiseLayer* conv12 = C2F(network, weightMap, *cat11->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.12");

    IResizeLayer* upsample13 = network->addResize(*conv12->getOutput(0));
    assert(upsample13);
    upsample13->setResizeMode(ResizeMode::kNEAREST);
    upsample13->setScales(scale, 3);

    ITensor* inputTensor14[] = {upsample13->getOutput(0), conv4->getOutput(0)};
    IConcatenationLayer* cat14 = network->addConcatenation(inputTensor14, 2);
    IElementWiseLayer* conv15 = C2F(network, weightMap, *cat14->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.15");
    IElementWiseLayer* conv16 = convBnSiLU(network, weightMap, *conv15->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.16");
    ITensor* inputTensor17[] = {conv16->getOutput(0), conv12->getOutput(0)};
    IConcatenationLayer* cat17 = network->addConcatenation(inputTensor17, 2);
    IElementWiseLayer* conv18 = C2F(network, weightMap, *cat17->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.18");
    IElementWiseLayer* conv19 = convBnSiLU(network, weightMap, *conv18->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.19");
    ITensor* inputTensor20[] = {conv19->getOutput(0), conv9->getOutput(0)};
    IConcatenationLayer* cat20 = network->addConcatenation(inputTensor20, 2);
    IElementWiseLayer* conv21 = C2F(network, weightMap, *cat20->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.21");

    /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
    int base_in_channel = (gw == 1.25) ? 80 : 64;
    int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(256, gw, max_channels);

    // output0
    IElementWiseLayer* conv22_cv2_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.0");
    IElementWiseLayer* conv22_cv2_0_1 = convBnSiLU(network, weightMap, *conv22_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.1");
    IConvolutionLayer* conv22_cv2_0_2 = network->addConvolutionNd(*conv22_cv2_0_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.0.2.weight"], weightMap["model.22.cv2.0.2.bias"]);
    conv22_cv2_0_2->setStrideNd(DimsHW{1, 1});
    conv22_cv2_0_2->setPaddingNd(DimsHW{0, 0});
    IElementWiseLayer* conv22_cv3_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0),base_out_channel, 3, 1, 1, "model.22.cv3.0.0");
    IElementWiseLayer* conv22_cv3_0_1 = convBnSiLU(network, weightMap, *conv22_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.1");
    IConvolutionLayer* conv22_cv3_0_2 = network->addConvolutionNd(*conv22_cv3_0_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.0.2.weight"], weightMap["model.22.cv3.0.2.bias"]);
    conv22_cv3_0_2->setStride(DimsHW{1, 1});
    conv22_cv3_0_2->setPadding(DimsHW{0, 0});
    ITensor* inputTensor22_0[] = {conv22_cv2_0_2->getOutput(0), conv22_cv3_0_2->getOutput(0)};
    IConcatenationLayer* cat22_0 = network->addConcatenation(inputTensor22_0, 2);

    // output1
    IElementWiseLayer* conv22_cv2_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.0");
    IElementWiseLayer* conv22_cv2_1_1 = convBnSiLU(network, weightMap, *conv22_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.1");
    IConvolutionLayer* conv22_cv2_1_2 = network->addConvolutionNd(*conv22_cv2_1_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.1.2.weight"], weightMap["model.22.cv2.1.2.bias"]);
    conv22_cv2_1_2->setStrideNd(DimsHW{1, 1});
    conv22_cv2_1_2->setPaddingNd(DimsHW{0, 0});
    IElementWiseLayer* conv22_cv3_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.0");
    IElementWiseLayer* conv22_cv3_1_1 = convBnSiLU(network, weightMap, *conv22_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.1");
    IConvolutionLayer* conv22_cv3_1_2 = network->addConvolutionNd(*conv22_cv3_1_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.1.2.weight"], weightMap["model.22.cv3.1.2.bias"]);
    conv22_cv3_1_2->setStrideNd(DimsHW{1, 1});
    conv22_cv3_1_2->setPaddingNd(DimsHW{0, 0});
    ITensor* inputTensor22_1[] = {conv22_cv2_1_2->getOutput(0), conv22_cv3_1_2->getOutput(0)};
    IConcatenationLayer* cat22_1 = network->addConcatenation(inputTensor22_1, 2);

    // output2
    IElementWiseLayer* conv22_cv2_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.0");
    IElementWiseLayer* conv22_cv2_2_1 = convBnSiLU(network, weightMap, *conv22_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.1");
    IConvolutionLayer* conv22_cv2_2_2 = network->addConvolution(*conv22_cv2_2_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.2.2.weight"], weightMap["model.22.cv2.2.2.bias"]);
    IElementWiseLayer* conv22_cv3_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.0");
    IElementWiseLayer* conv22_cv3_2_1 = convBnSiLU(network, weightMap, *conv22_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.1");
    IConvolutionLayer* conv22_cv3_2_2 = network->addConvolution(*conv22_cv3_2_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.2.2.weight"], weightMap["model.22.cv3.2.2.bias"]);
    ITensor* inputTensor22_2[] = {conv22_cv2_2_2->getOutput(0), conv22_cv3_2_2->getOutput(0)};
    IConcatenationLayer* cat22_2 = network->addConcatenation(inputTensor22_2, 2);

    /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/

    IShuffleLayer* shuffle22_0 = network->addShuffle(*cat22_0->getOutput(0));
    shuffle22_0->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});

    ISliceLayer* split22_0_0 = network->addSlice(*shuffle22_0->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 8) * (kInputW / 8)}, Dims2{1, 1});
    ISliceLayer* split22_0_1 = network->addSlice(*shuffle22_0->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, Dims2{1, 1});
    IShuffleLayer* dfl22_0 = DFL(network, weightMap, *split22_0_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.22.dfl.conv.weight");
    ITensor* inputTensor22_dfl_0[] = {dfl22_0->getOutput(0), split22_0_1->getOutput(0)};
    IConcatenationLayer* cat22_dfl_0 = network->addConcatenation(inputTensor22_dfl_0, 2);

    IShuffleLayer* shuffle22_1 = network->addShuffle(*cat22_1->getOutput(0));
    shuffle22_1->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
    ISliceLayer* split22_1_0 = network->addSlice(*shuffle22_1->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 16) * (kInputW / 16)}, Dims2{1, 1});
    ISliceLayer* split22_1_1 = network->addSlice(*shuffle22_1->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, Dims2{1, 1});
    IShuffleLayer* dfl22_1 = DFL(network, weightMap, *split22_1_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.22.dfl.conv.weight");
    ITensor* inputTensor22_dfl_1[] = {dfl22_1->getOutput(0), split22_1_1->getOutput(0)};
    IConcatenationLayer* cat22_dfl_1 = network->addConcatenation(inputTensor22_dfl_1, 2);

    IShuffleLayer* shuffle22_2 = network->addShuffle(*cat22_2->getOutput(0));
    shuffle22_2->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
    ISliceLayer* split22_2_0 = network->addSlice(*shuffle22_2->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 32) * (kInputW / 32)}, Dims2{1, 1});
    ISliceLayer* split22_2_1 = network->addSlice(*shuffle22_2->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, Dims2{1, 1});
    IShuffleLayer* dfl22_2 = DFL(network, weightMap, *split22_2_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.22.dfl.conv.weight");
    ITensor* inputTensor22_dfl_2[] = {dfl22_2->getOutput(0), split22_2_1->getOutput(0)};
    IConcatenationLayer* cat22_dfl_2 = network->addConcatenation(inputTensor22_dfl_2, 2);

    IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<IConcatenationLayer *>{cat22_dfl_0, cat22_dfl_1, cat22_dfl_2});
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));

    #if defined(USE_FP16)
      config->setFlag(BuilderFlag::kFP16);
    #elif defined(USE_INT8)
      std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
      assert(builder->platformHasFastInt8());
      config->setFlag(BuilderFlag::kINT8);
      Int8EntropyCalibrator2* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "./coco_calib/", "int8calib.table", kInputTensorName);
      config->setInt8Calibrator(calibrator);
    #endif

    std::cout << "Building engine, please wait for a while..." << std::endl;
    IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    delete network;

    for (auto &mem : weightMap){
        free((void *)(mem.second.values));
    }
    return serialized_model;
}

IHostMemory* buildEngineYolov8Seg(IBuilder* builder,
                                            IBuilderConfig* config, DataType dt,
                                            const std::string& wts_path, float& gd, float& gw, int& max_channels) {
    std::map<std::string, Weights> weightMap = loadWeights(wts_path);
    INetworkDefinition* network = builder->createNetworkV2(0U);

    /*******************************************************************************************************
    ******************************************  YOLOV8 INPUT  **********************************************
    *******************************************************************************************************/
    ITensor* data = network->addInput(kInputTensorName, dt, Dims3{3, kInputH, kInputW});
    assert(data);

    /*******************************************************************************************************
    *****************************************  YOLOV8 BACKBONE  ********************************************
    *******************************************************************************************************/
    IElementWiseLayer* conv0 = convBnSiLU(network, weightMap, *data, get_width(64, gw, max_channels), 3, 2, 1, "model.0");
    IElementWiseLayer* conv1 = convBnSiLU(network, weightMap, *conv0->getOutput(0), get_width(128, gw, max_channels), 3, 2, 1, "model.1");
    IElementWiseLayer* conv2 = C2F(network, weightMap, *conv1->getOutput(0), get_width(128, gw, max_channels), get_width(128, gw, max_channels), get_depth(3, gd), true, 0.5, "model.2");
    IElementWiseLayer* conv3 = convBnSiLU(network, weightMap, *conv2->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.3");
    IElementWiseLayer* conv4 = C2F(network, weightMap, *conv3->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(6, gd), true, 0.5, "model.4");
    IElementWiseLayer* conv5 = convBnSiLU(network, weightMap, *conv4->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.5");
    IElementWiseLayer* conv6 = C2F(network, weightMap, *conv5->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(6, gd), true, 0.5, "model.6");
    IElementWiseLayer* conv7 = convBnSiLU(network, weightMap, *conv6->getOutput(0), get_width(1024, gw, max_channels), 3, 2, 1, "model.7");
    IElementWiseLayer* conv8 = C2F(network, weightMap, *conv7->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), true, 0.5, "model.8");
    IElementWiseLayer* conv9 = SPPF(network, weightMap, *conv8->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), 5, "model.9");

    /*******************************************************************************************************
    *********************************************  YOLOV8 HEAD  ********************************************
    *******************************************************************************************************/
    float scale[] = {1.0, 2.0, 2.0};
    IResizeLayer* upsample10 = network->addResize(*conv9->getOutput(0));
    assert(upsample10);
    upsample10->setResizeMode(ResizeMode::kNEAREST);
    upsample10->setScales(scale, 3);

    ITensor* inputTensor11[] = {upsample10->getOutput(0), conv6->getOutput(0)};
    IConcatenationLayer* cat11 = network->addConcatenation(inputTensor11, 2);
    IElementWiseLayer* conv12 = C2F(network, weightMap, *cat11->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.12");

    IResizeLayer* upsample13 = network->addResize(*conv12->getOutput(0));
    assert(upsample13);
    upsample13->setResizeMode(ResizeMode::kNEAREST);
    upsample13->setScales(scale, 3);

    ITensor* inputTensor14[] = {upsample13->getOutput(0), conv4->getOutput(0)};
    IConcatenationLayer* cat14 = network->addConcatenation(inputTensor14, 2);
    IElementWiseLayer* conv15 = C2F(network, weightMap, *cat14->getOutput(0), get_width(256, gw, max_channels), get_width(256, gw, max_channels), get_depth(3, gd), false, 0.5, "model.15");
    IElementWiseLayer* conv16 = convBnSiLU(network, weightMap, *conv15->getOutput(0), get_width(256, gw, max_channels), 3, 2, 1, "model.16");
    ITensor* inputTensor17[] = {conv16->getOutput(0), conv12->getOutput(0)};
    IConcatenationLayer* cat17 = network->addConcatenation(inputTensor17, 2);
    IElementWiseLayer* conv18 = C2F(network, weightMap, *cat17->getOutput(0), get_width(512, gw, max_channels), get_width(512, gw, max_channels), get_depth(3, gd), false, 0.5, "model.18");
    IElementWiseLayer* conv19 = convBnSiLU(network, weightMap, *conv18->getOutput(0), get_width(512, gw, max_channels), 3, 2, 1, "model.19");
    ITensor* inputTensor20[] = {conv19->getOutput(0), conv9->getOutput(0)};
    IConcatenationLayer* cat20 = network->addConcatenation(inputTensor20, 2);
    IElementWiseLayer* conv21 = C2F(network, weightMap, *cat20->getOutput(0), get_width(1024, gw, max_channels), get_width(1024, gw, max_channels), get_depth(3, gd), false, 0.5, "model.21");

    /*******************************************************************************************************
    *********************************************  YOLOV8 OUTPUT  ******************************************
    *******************************************************************************************************/
    int base_in_channel = (gw == 1.25) ? 80 : 64;
    int base_out_channel = (gw == 0.25) ? std::max(64, std::min(kNumClass, 100)) : get_width(256, gw, max_channels);

    // output0
    IElementWiseLayer* conv22_cv2_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.0");
    IElementWiseLayer* conv22_cv2_0_1 = convBnSiLU(network, weightMap, *conv22_cv2_0_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.0.1");
    IConvolutionLayer* conv22_cv2_0_2 = network->addConvolutionNd(*conv22_cv2_0_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.0.2.weight"], weightMap["model.22.cv2.0.2.bias"]);
    conv22_cv2_0_2->setStrideNd(DimsHW{1, 1});
    conv22_cv2_0_2->setPaddingNd(DimsHW{0, 0});
    IElementWiseLayer *conv22_cv3_0_0 = convBnSiLU(network, weightMap, *conv15->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.0");
    IElementWiseLayer *conv22_cv3_0_1 = convBnSiLU(network, weightMap, *conv22_cv3_0_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.0.1");
    IConvolutionLayer *conv22_cv3_0_2 = network->addConvolutionNd(*conv22_cv3_0_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.0.2.weight"], weightMap["model.22.cv3.0.2.bias"]);
    conv22_cv3_0_2->setStride(DimsHW{1, 1});
    conv22_cv3_0_2->setPadding(DimsHW{0, 0});
    ITensor* inputTensor22_0[] = {conv22_cv2_0_2->getOutput(0), conv22_cv3_0_2->getOutput(0)};
    IConcatenationLayer* cat22_0 = network->addConcatenation(inputTensor22_0, 2);

    // output1
    IElementWiseLayer* conv22_cv2_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.0");
    IElementWiseLayer* conv22_cv2_1_1 = convBnSiLU(network, weightMap, *conv22_cv2_1_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.1.1");
    IConvolutionLayer* conv22_cv2_1_2 = network->addConvolutionNd(*conv22_cv2_1_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.1.2.weight"], weightMap["model.22.cv2.1.2.bias"]);
    conv22_cv2_1_2->setStrideNd(DimsHW{1, 1});
    conv22_cv2_1_2->setPaddingNd(DimsHW{0, 0});
    IElementWiseLayer* conv22_cv3_1_0 = convBnSiLU(network, weightMap, *conv18->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.0");
    IElementWiseLayer* conv22_cv3_1_1 = convBnSiLU(network, weightMap, *conv22_cv3_1_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.1.1");
    IConvolutionLayer* conv22_cv3_1_2 = network->addConvolutionNd(*conv22_cv3_1_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.1.2.weight"], weightMap["model.22.cv3.1.2.bias"]);
    conv22_cv3_1_2->setStrideNd(DimsHW{1, 1});
    conv22_cv3_1_2->setPaddingNd(DimsHW{0, 0});
    ITensor* inputTensor22_1[] = {conv22_cv2_1_2->getOutput(0), conv22_cv3_1_2->getOutput(0)};
    IConcatenationLayer* cat22_1 = network->addConcatenation(inputTensor22_1, 2);

    // output2
    IElementWiseLayer* conv22_cv2_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.0");
    IElementWiseLayer* conv22_cv2_2_1 = convBnSiLU(network, weightMap, *conv22_cv2_2_0->getOutput(0), base_in_channel, 3, 1, 1, "model.22.cv2.2.1");
    IConvolutionLayer* conv22_cv2_2_2 = network->addConvolution(*conv22_cv2_2_1->getOutput(0), 64, DimsHW{1, 1}, weightMap["model.22.cv2.2.2.weight"], weightMap["model.22.cv2.2.2.bias"]);
    IElementWiseLayer* conv22_cv3_2_0 = convBnSiLU(network, weightMap, *conv21->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.0");
    IElementWiseLayer* conv22_cv3_2_1 = convBnSiLU(network, weightMap, *conv22_cv3_2_0->getOutput(0), base_out_channel, 3, 1, 1, "model.22.cv3.2.1");
    IConvolutionLayer* conv22_cv3_2_2 = network->addConvolution(*conv22_cv3_2_1->getOutput(0), kNumClass, DimsHW{1, 1}, weightMap["model.22.cv3.2.2.weight"], weightMap["model.22.cv3.2.2.bias"]);
    ITensor* inputTensor22_2[] = {conv22_cv2_2_2->getOutput(0), conv22_cv3_2_2->getOutput(0)};
    IConcatenationLayer* cat22_2 = network->addConcatenation(inputTensor22_2, 2);

    /*******************************************************************************************************
    *********************************************  YOLOV8 DETECT  ******************************************
    *******************************************************************************************************/

    IShuffleLayer* shuffle22_0 = network->addShuffle(*cat22_0->getOutput(0));
    shuffle22_0->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 8) * (kInputW / 8)});

    ISliceLayer* split22_0_0 = network->addSlice(*shuffle22_0->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 8) * (kInputW / 8)}, Dims2{1, 1});
    ISliceLayer* split22_0_1 = network->addSlice(*shuffle22_0->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 8) * (kInputW / 8)}, Dims2{1, 1});
    IShuffleLayer* dfl22_0 = DFL(network, weightMap, *split22_0_0->getOutput(0), 4, (kInputH / 8) * (kInputW / 8), 1, 1, 0, "model.22.dfl.conv.weight");

    IShuffleLayer* shuffle22_1 = network->addShuffle(*cat22_1->getOutput(0));
    shuffle22_1->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 16) * (kInputW / 16)});
    ISliceLayer* split22_1_0 = network->addSlice(*shuffle22_1->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 16) * (kInputW / 16)}, Dims2{1, 1});
    ISliceLayer* split22_1_1 = network->addSlice(*shuffle22_1->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 16) * (kInputW / 16)}, Dims2{1, 1});
    IShuffleLayer* dfl22_1 = DFL(network, weightMap, *split22_1_0->getOutput(0), 4, (kInputH / 16) * (kInputW / 16), 1, 1, 0, "model.22.dfl.conv.weight");

    IShuffleLayer* shuffle22_2 = network->addShuffle(*cat22_2->getOutput(0));
    shuffle22_2->setReshapeDimensions(Dims2{64 + kNumClass, (kInputH / 32) * (kInputW / 32)});
    ISliceLayer* split22_2_0 = network->addSlice(*shuffle22_2->getOutput(0), Dims2{0, 0}, Dims2{64, (kInputH / 32) * (kInputW / 32)}, Dims2{1, 1});
    ISliceLayer* split22_2_1 = network->addSlice(*shuffle22_2->getOutput(0), Dims2{64, 0}, Dims2{kNumClass, (kInputH / 32) * (kInputW / 32)}, Dims2{1, 1});
    IShuffleLayer* dfl22_2 = DFL(network, weightMap, *split22_2_0->getOutput(0), 4, (kInputH / 32) * (kInputW / 32), 1, 1, 0, "model.22.dfl.conv.weight");

    // det0
    auto proto_coef_0 = ProtoCoef(network, weightMap, *conv15->getOutput(0), "model.22.cv4.0", 6400, gw);
    ITensor* inputTensor22_dfl_0[] = { dfl22_0->getOutput(0), split22_0_1->getOutput(0),proto_coef_0->getOutput(0)};
    IConcatenationLayer *cat22_dfl_0 = network->addConcatenation(inputTensor22_dfl_0, 3);

    // det1
    auto proto_coef_1 = ProtoCoef(network, weightMap, *conv18->getOutput(0), "model.22.cv4.1", 1600, gw);
    ITensor* inputTensor22_dfl_1[] = { dfl22_1->getOutput(0), split22_1_1->getOutput(0),proto_coef_1->getOutput(0)};
    IConcatenationLayer *cat22_dfl_1 = network->addConcatenation(inputTensor22_dfl_1, 3);

    // det2
    auto proto_coef_2 = ProtoCoef(network, weightMap, *conv21->getOutput(0), "model.22.cv4.2", 400, gw);
    ITensor* inputTensor22_dfl_2[] = { dfl22_2->getOutput(0), split22_2_1->getOutput(0) ,proto_coef_2->getOutput(0)};
    IConcatenationLayer *cat22_dfl_2 = network->addConcatenation(inputTensor22_dfl_2, 3);


    IPluginV2Layer* yolo = addYoLoLayer(network, std::vector<IConcatenationLayer *>{cat22_dfl_0, cat22_dfl_1, cat22_dfl_2}, true);
    yolo->getOutput(0)->setName(kOutputTensorName);
    network->markOutput(*yolo->getOutput(0));

    auto proto = Proto(network, weightMap, *conv15->getOutput(0), "model.22.proto", gw, max_channels);
    proto->getOutput(0)->setName("proto");
    network->markOutput(*proto->getOutput(0));

    builder->setMaxBatchSize(kBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));




    #if defined(USE_FP16)
      config->setFlag(BuilderFlag::kFP16);
    #elif defined(USE_INT8)
      std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
      assert(builder->platformHasFastInt8());
      config->setFlag(BuilderFlag::kINT8);
      Int8EntropyCalibrator2* calibrator = new Int8EntropyCalibrator2(1, kInputW, kInputH, "./coco_calib/", "int8calib.table", kInputTensorName);
      config->setInt8Calibrator(calibrator);
    #endif


    std::cout << "Building engine, please wait for a while..." << std::endl;
    IHostMemory* serialized_model = builder->buildSerializedNetwork(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    delete network;

    for (auto& mem : weightMap) {
        free((void*)(mem.second.values));
    }
    return serialized_model;
}
