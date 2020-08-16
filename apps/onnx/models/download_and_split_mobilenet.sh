wget https://github.com/onnx/models/raw/master/vision/classification/mobilenet/model/mobilenetv2-7.onnx
mv mobilenetv2-7.onnx mobilenetv2.onnx
python splitter.py mobilenetv2.onnx mobilenetv2_0.onnx --inputs mobilenetv20_features_conv0_weight --outputs mobilenetv20_features_linearbottleneck1_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_1.onnx --inputs mobilenetv20_features_linearbottleneck1_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck3_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_2.onnx --inputs mobilenetv20_features_linearbottleneck3_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck5_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_3.onnx --inputs mobilenetv20_features_linearbottleneck5_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck7_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_4.onnx --inputs mobilenetv20_features_linearbottleneck7_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck9_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_5.onnx --inputs mobilenetv20_features_linearbottleneck9_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck11_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_6.onnx --inputs mobilenetv20_features_linearbottleneck11_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck13_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_7.onnx --inputs mobilenetv20_features_linearbottleneck13_batchnorm2_fwd --outputs mobilenetv20_features_linearbottleneck15_batchnorm2_fwd
python splitter.py mobilenetv2.onnx mobilenetv2_8.onnx --inputs mobilenetv20_features_linearbottleneck15_batchnorm2_fwd --outputs mobilenetv20_output_flatten0_reshape0
