from .llm import LLMConfig, EXAMPLE_INPUTS_MODE

model_id_list = ["EleutherAI/gpt-j-6b"]

class GPTJConfig(LLMConfig):
    def __init__(self, model_id):
        self.name = "gpt-j"
        assert model_id in model_id_list, "%s is not a %s model" % (model_id, self.name)
        self.model_id = model_id
        self.to_channels_last = True
        self.trust_remote_code = False
        self.example_inputs_mode = EXAMPLE_INPUTS_MODE.MASK_POS_KV

        # for smooth quant
        self.default_dataset = "NeelNanda/pile-10k"
        self.use_global_past_key_value = False
        self.use_neural_compressor = True
