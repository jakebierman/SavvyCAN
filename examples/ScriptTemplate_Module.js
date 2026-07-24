module.exports = {
    hex: function(value)
    {
        return "0x" + value.toString(16).toUpperCase();
    },

    bitSet: function(value, bit)
    {
        return ((value >> bit) & 1) !== 0;
    }
};
